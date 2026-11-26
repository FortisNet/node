#!/usr/bin/env python3
"""R52: replay the frozen TypeScript POST /v1/sync edges
(tests/corpus/sync_edges.json, written by
the earlier TypeScript implementation's tests and checked against its
SHA-256 pin) against build/fortis_node_address_route_test over real
sockets, then run the native-only concurrency and crash checks.

Replay. Each fixture group runs in a fresh process over one KV directory;
the previous process is SIGKILLed first (a hard restart: only what reached
disk survives). Seeds go through the probes (`append`:
/probe/test-sync-append, one journal frame; `watch`: /probe/test-sync-watch,
WatchStore.put for the bearer's device). Every request runs at its fixture
clock; status, the fixture header set and the exact body bytes (JSON text or
protobuf wire) must match. x-rate-limit-remaining is compared only in the
`quota` group, after the native device B has been drained to 1,200 in a
fresh wall-clock minute (the native per-minute window is the wall clock).
`concurrent` cases send their copies at once; every answer must equal the
recorded one.

Asserted native differences (each checked, never skipped):
  jsonTooLargeForNative  the request JSON parser's 256 KiB bound:
                         413 {"error":"BODY_TOO_LARGE"} after the device
                         policy (charged; fastify-errors).
  methodGet/methodPut/pathTrailingSlash/pathUpperCase
                         the router's own 404 body (route misses are
                         router-wide, as in R35/R51).

Native-only checks (no TypeScript equivalent):
  concurrency  four writers append single-event frames to four watched and
               one unwatched account while eight readers page device A from
               the zero cursor (every page ordered and inside the scope;
               repeated reads of one cursor only grow, each answer a prefix
               of the next) and two followers poll on the cursors they are
               given (every watched event exactly once, in order, never a
               reset); the final read sees every watched event exactly once.
  crash        writers append while the process is SIGKILLed; after a restart
               the watched journal is exactly 1..head with no gap, a cursor
               issued before the crash continues, and the next append takes
               head + 1.

Usage: sync_edges_test.py <address route test binary>
Environment: SYNC_EDGES_ONLY_GROUPS=main,quota limits the replay;
SYNC_EDGES_SKIP_NATIVE=1 skips the native-only checks.
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

import address_routes_test as replay
import compat_expectations
import discovery_route_harness as harness

FIXTURE = "sync_edges.json"
NET = "eth-mainnet"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after"]
BODY_TOO_LARGE = b'{"error":"BODY_TOO_LARGE"}'
ROUTE_MISSES = {"methodGet", "methodPut", "pathTrailingSlash", "pathUpperCase"}
# auth-first (fastify-errors): the device policy refuses these before
# the body is judged (the quota is spent, or attestation is required).
POLICY_FIRST = {"quotaOverSchemaInvalid": (429, b'{"error":"RATE_LIMITED"}'),
                "attestationSchemaFirst": (401, b'{"error":"ATTESTATION_REQUIRED"}')}
FAILURES = []


def fail(message):
    FAILURES.append(message)
    print("FAIL:", message, flush=True)


def load_fixture():
    path = harness.FIXTURES / FIXTURE
    data = path.read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()
    digest = hashlib.sha256(data).hexdigest()
    if pinned != [digest, FIXTURE]:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pin {pinned}")
    return json.loads(data)


def request_body(case):
    if "requestHex" in case:
        return bytes.fromhex(case["requestHex"])
    if "requestJson" in case:
        document = json.loads(case["requestJson"])
        document["pad"] = "p" * int(case["pad"])
        return json.dumps(document, separators=(",", ":")).encode()
    return b""


def raw_request(port, method, target, headers, body, timeout=60):
    """One HTTP/1.1 exchange; the declared Content-Length is the body's."""
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers.items()]
    if body or method == "POST":
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + body)
        data = bytearray()
        while True:
            try:
                chunk = sock.recv(65536)
            except ConnectionResetError:
                break
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    response = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            response[key.lower()] = value.strip()
    return int(first.split()[1]), response, payload


def expected_payload(case):
    if "wireHex" in case:
        return bytes.fromhex(case["wireHex"])
    return case["bodyText"].encode("utf-8", "surrogatepass")


def compare(case, status, headers, payload, remaining):
    name = case["name"]
    ce = compat_expectations
    # compat-decisions.md#form-bodies, #fastify-errors: the body
    # parser's or the schema's refusal after the device policy (charged), or
    # the policy's own refusal for a request without a device credential.
    if name in POLICY_FIRST:
        # auth-first: the device policy refuses before the body is judged.
        want_status, want_body = POLICY_FIRST[name]
        if status != want_status or payload != want_body:
            fail(f"{name}: the device policy does not answer first: {status} {payload[:120]!r}")
            return False
        return True
    removed = ce.is_removed_body_media(case.get("requestHeaders", {}).get("content-type"))
    if removed or ce.fastify_code(case.get("bodyText")):
        want_status, _, want_body, charged = ce.fastify_expected(
            case["status"], {}, ce.UNSUPPORTED_MEDIA_TYPE_FASTIFY if removed else case["bodyText"],
            case.get("requestHeaders", {}))
        if status != want_status or payload != want_body or ("x-rate-limit-cost" in headers) != charged:
            fail(f"{name}: body framing: {status} {payload[:200]!r} is not {want_status} {want_body!r}")
            return False
        return True
    # zod-issues: a recorded issue list is {"error":"BAD_REQUEST"} here.
    if "bodyText" in case:
        _, mapped_headers, mapped_body = ce.zod_expected(case["status"], case["headers"], case["bodyText"])
        if mapped_headers is not case["headers"]:
            case = dict(case, headers=mapped_headers, bodyText=mapped_body)
    ok = True
    if status != case["status"]:
        fail(f"{name}: status {status} != {case['status']} ({payload[:300]!r})")
        return False
    for header in HEADERS:
        if header == "x-rate-limit-remaining" and not remaining:
            continue
        if headers.get(header) != case["headers"].get(header):
            fail(f"{name}: header {header}: {headers.get(header)!r} != {case['headers'].get(header)!r}")
            ok = False
    expected = expected_payload(case)
    if payload != expected:
        index = next((i for i, (x, y) in enumerate(zip(expected, payload)) if x != y),
                     min(len(expected), len(payload)))
        fail(f"{name}: body differs at byte {index} (want {len(expected)}, got {len(payload)})\n"
             f"  want …{expected[max(0, index - 120):index + 120]!r}\n"
             f"  got  …{payload[max(0, index - 120):index + 120]!r}")
        ok = False
    return ok


def run_seed(server, fixture, case):
    if case["seed"] == "append":
        status, _, payload = server.request("POST", "/probe/test-sync-append", body=json.dumps(case["input"]).encode(),
                                            headers={"Content-Type": "application/json"})
        if status != 200 or json.loads(payload or b"null") != case["result"]:
            fail(f"{case['name']}: append {status} {payload[:200]!r} != {case['result']}")
        return
    seed = case["input"]
    status, _, payload = server.request(
        "POST", "/probe/test-sync-watch",
        body=json.dumps({"topic": seed["topic"], "account": seed["account"], "expiresAt": seed["expiresAt"]}).encode(),
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {fixture['tokens'][seed['device']]}"})
    if status != 200:
        fail(f"{case['name']}: watch {status} {payload[:200]!r}")


def run_case(server, case, remaining, tally):
    name = case["name"]
    body = request_body(case)
    headers = dict(case["requestHeaders"])
    if "concurrent" in case:
        answers = [None] * case["concurrent"]

        def one(index):
            answers[index] = raw_request(server.port, case["method"], case["url"], headers, body)

        threads = [threading.Thread(target=one, args=(i,)) for i in range(case["concurrent"])]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        results = [compare(case, *answer, remaining) for answer in answers]
        tally["matched" if all(results) else "failed"] += 1
        return
    status, response, payload = raw_request(server.port, case["method"], case["url"], headers, body)
    if name == "modelsScalarCoerced":
        ok = status == 400 and payload == b'{"error":"BAD_REQUEST"}' and "x-rate-limit-cost" in response
        if not ok:
            fail(f"{name}: strict array type refusal: {status} {payload[:200]!r}")
        tally["asserted" if ok else "failed"] += 1
        return
    if name == "jsonTooLargeForNative":
        ok = status == 413 and payload == BODY_TOO_LARGE and "x-rate-limit-cost" in response
        if not ok:
            fail(f"{name}: native 256 KiB parser bound: {status} {payload[:200]!r}")
        tally["asserted" if ok else "failed"] += 1
        return
    if name in ROUTE_MISSES:
        ok = status == 404 and "x-rate-limit-cost" not in response and case["status"] == 404
        if not ok:
            fail(f"{name}: router miss: {status} {payload[:200]!r}")
        tally["asserted" if ok else "failed"] += 1
        return
    tally["matched" if compare(case, status, response, payload, remaining) else "failed"] += 1


def crash(server):
    server.process.send_signal(signal.SIGKILL)
    server.process.wait()


def drain_quota(server, token, count):
    for _ in range(count):
        status, _, payload = raw_request(server.port, "POST", "/v1/sync",
                                         {"authorization": f"Bearer {token}", "content-type": "application/json"}, b"{}")
        if status != 200:
            fail(f"quota drain answered {status} {payload[:120]!r}")
            return


def replay_fixture(binary, root, upstream, fixture):
    only = set(filter(None, os.environ.get("SYNC_EDGES_ONLY_GROUPS", "").split(",")))
    kv_dir = root / "kv"
    kv_dir.mkdir()
    clock = root / "clock"
    by_group = {}
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = {"matched": 0, "asserted": 0, "failed": 0, "seeds": 0}
    server = None
    try:
        for group in fixture["groups"]:
            name = group["name"]
            cases = by_group.get(name, [])
            harness.set_clock(clock, group["start"])
            if server is not None:
                crash(server)
            server = replay.Server(binary, kv_dir, clock, upstream, group["env"])
            if only and name not in only:
                # Seeds still run so later groups see the same journal.
                for case in cases:
                    if "seed" in case:
                        harness.set_clock(clock, case["at"])
                        run_seed(server, fixture, case)
                continue
            tally = {"matched": 0, "asserted": 0, "failed": 0, "seeds": 0}
            if name == "quota":
                wait_for_fresh_minute(40)
                drain_quota(server, fixture["tokens"]["B"], 1200)
            for case in cases:
                harness.set_clock(clock, case["at"])
                if "seed" in case:
                    run_seed(server, fixture, case)
                    tally["seeds"] += 1
                    continue
                run_case(server, case, name == "quota", tally)
            for key in totals:
                totals[key] += tally[key]
            print(f"{name}: {tally['matched']} matched, {tally['asserted']} asserted differences, "
                  f"{tally['failed']} failed ({tally['seeds']} seeds)", flush=True)
        if server is not None:
            server.stop()
    finally:
        if server is not None and server.process.poll() is None:
            server.process.kill()
    return totals


def wait_for_fresh_minute(budget_seconds):
    second = time.time() % 60
    if second > 60 - budget_seconds:
        time.sleep(60 - second + 0.3)


# ---- native-only ------------------------------------------------------------------------------

TOKEN_A = "fintel_dev_" + "A" * 43
ACCOUNTS = [f"0x{'c' * 38}{index:02d}" for index in range(4)]
OTHER = "0x" + "d" * 40


class Client:
    def __init__(self, server):
        self.server = server

    def append(self, address, events):
        body = json.dumps({"network": NET, "address": address, "events": events, "rollback": False}).encode()
        status, _, payload = self.server.request("POST", "/probe/test-sync-append", body=body,
                                                 headers={"Content-Type": "application/json"})
        return status, json.loads(payload or b"null")

    def watch(self, address, expires):
        body = json.dumps({"topic": f"address:{NET}:{address}", "account": None, "expiresAt": expires}).encode()
        status, _, _ = self.server.request("POST", "/probe/test-sync-watch", body=body,
                                           headers={"Content-Type": "application/json",
                                                    "Authorization": f"Bearer {TOKEN_A}"})
        return status

    def sync(self, cursor, limit, protobuf=False):
        status, headers, payload = raw_request(
            self.server.port, "POST", "/v1/sync",
            {"authorization": f"Bearer {TOKEN_A}", "content-type": "application/json",
             "accept": "application/json"}, json.dumps({"cursor": cursor, "limit": limit}).encode())
        return status, (json.loads(payload) if status == 200 else payload)


def event(entity, at):
    return {"entityKey": {"id": f"{NET}:{entity}"}, "operation": "upsert", "fields": {"id": entity}, "changedAt": at}


def seq_of(change):
    return int(change["version"][3:])


def read_all(client, cursor, limit):
    """Pages from `cursor` until hasMore is false: (changes, last cursor)."""
    changes = []
    for _ in range(10000):
        status, page = client.sync(cursor, limit)
        if status != 200:
            return None, f"status {status} {page[:120]!r}"
        if page["reset"]:
            return None, f"reset at {cursor}"
        changes.extend(page["changes"])
        cursor = page["cursor"]
        if not page["hasMore"]:
            return changes, cursor
    return None, "did not finish"


def native_concurrency(binary, root, upstream):
    kv_dir = root / "kv-concurrency"
    kv_dir.mkdir()
    clock = root / "clock-concurrency"
    now = 1767323045000
    harness.set_clock(clock, now)
    server = replay.Server(binary, kv_dir, clock, upstream, {})
    client = Client(server)
    try:
        for address in ACCOUNTS:
            if client.append(address, [])[0] != 200 or client.watch(address, now + 3600_000) != 200:
                fail("concurrency: seed failed")
                return
        zero = f"s2_{now:013d}_{'0' * 20}"
        per_writer = 60
        stop = threading.Event()
        errors = []
        observed = []

        def writer(address, label):
            for index in range(per_writer):
                status, _ = client.append(address, [event(f"{label}-{index}", now)])
                if status != 200:
                    errors.append(f"append {label} {status}")
                    return

        def reader(index):
            last = []
            while not stop.is_set():
                status, page = client.sync(zero, 500)
                if status != 200 or page["reset"]:
                    errors.append(f"reader {index}: {status} {page if status != 200 else 'reset'}")
                    return
                versions = [seq_of(change) for change in page["changes"]]
                if versions != sorted(versions) or len(set(versions)) != len(versions):
                    errors.append(f"reader {index}: unordered page")
                    return
                if any(change["key"]["address"] not in ACCOUNTS for change in page["changes"]):
                    errors.append(f"reader {index}: change outside the scope")
                    return
                if not page["hasMore"] and versions[:len(last)] != last:
                    errors.append(f"reader {index}: a later read is not an extension of an earlier one")
                    return
                if not page["hasMore"]:
                    last = versions
                observed.append(len(versions))

        followed = {}

        def follower(index):
            # The app's poll: follow the returned cursors while writes land;
            # every event arrives once, in order, with no reset.
            cursor, last, seen = zero, 0, []
            idle = 0
            while idle < 2:
                status, page = client.sync(cursor, 9)
                if status != 200 or page["reset"]:
                    errors.append(f"follower {index}: {status} {page if status != 200 else 'reset'}")
                    return
                for change in page["changes"]:
                    if seq_of(change) <= last:
                        errors.append(f"follower {index}: {change['version']} after {last}")
                        return
                    last = seq_of(change)
                    seen.append(change["key"]["id"])
                cursor = page["cursor"]
                idle = idle + 1 if stop.is_set() and not page["changes"] and not page["hasMore"] else 0
            followed[index] = seen

        writers = [threading.Thread(target=writer, args=(address, f"w{i}")) for i, address in enumerate(ACCOUNTS)]
        writers.append(threading.Thread(target=writer, args=(OTHER, "other")))
        readers = [threading.Thread(target=reader, args=(i,)) for i in range(8)]
        followers = [threading.Thread(target=follower, args=(i,)) for i in range(2)]
        for thread in readers + followers + writers:
            thread.start()
        for thread in writers:
            thread.join()
        stop.set()
        for thread in readers + followers:
            thread.join()
        for error in errors[:5]:
            fail(f"concurrency: {error}")
        changes, _ = read_all(client, zero, 7)
        if changes is None:
            fail("concurrency: final paging failed")
            return
        entities = [change["key"]["id"] for change in changes]
        expected = {f"{NET}:w{i}-{n}" for i in range(4) for n in range(per_writer)}
        if len(entities) != len(set(entities)) or set(entities) != expected:
            fail(f"concurrency: final read has {len(entities)} changes ({len(set(entities))} distinct), "
                 f"want {len(expected)}")
        versions = [seq_of(change) for change in changes]
        if versions != sorted(versions):
            fail("concurrency: final read out of order")
        for index, seen in sorted(followed.items()):
            if len(seen) != len(set(seen)) or set(seen) != expected:
                fail(f"concurrency: follower {index} saw {len(seen)} changes ({len(set(seen))} distinct), "
                     f"want {len(expected)}")
        if len(followed) != 2:
            fail("concurrency: a follower did not finish")
        print(f"native concurrency: {len(errors)} errors, {len(observed)} concurrent reads, 2 cursor followers, "
              f"final {len(entities)} watched changes of {5 * per_writer} appended", flush=True)
    finally:
        server.stop()


def native_crash(binary, root, upstream):
    kv_dir = root / "kv-crash"
    kv_dir.mkdir()
    clock = root / "clock-crash"
    now = 1767323045000
    harness.set_clock(clock, now)
    server = replay.Server(binary, kv_dir, clock, upstream, {})
    client = Client(server)
    address = ACCOUNTS[0]
    if client.append(address, []) != (200, {"head": "0" * 20}) or client.watch(address, now + 3600_000) != 200:
        fail("crash: seed failed")
        crash(server)
        return
    for index in range(5):
        client.append(address, [event(f"pre-{index}", now)])
    status, before = client.sync(f"s2_{now:013d}_{'0' * 20}", 500)
    early_cursor = before["cursor"] if status == 200 else None
    stop = threading.Event()
    appended = [0]

    def writer(label):
        index = 0
        while not stop.is_set():
            try:
                status, _ = client.append(address, [event(f"{label}-{index}", now), event(f"{label}-{index}b", now)])
            except (OSError, IndexError, ValueError):
                return  # the process was killed mid-exchange
            if status == 200:
                appended[0] += 2
            index += 1

    threads = [threading.Thread(target=writer, args=(f"c{i}",)) for i in range(3)]
    for thread in threads:
        thread.start()
    time.sleep(0.6)
    crash(server)
    stop.set()
    for thread in threads:
        thread.join()
    harness.set_clock(clock, now + 1000)
    server = replay.Server(binary, kv_dir, clock, upstream, {})
    client = Client(server)
    try:
        changes, _ = read_all(client, f"s2_{now + 1000:013d}_{'0' * 20}", 500)
        if changes is None:
            fail("crash: reading after the restart failed")
            return
        versions = [seq_of(change) for change in changes]
        head = versions[-1] if versions else 0
        if versions != list(range(1, head + 1)):
            fail(f"crash: journal after restart is not 1..{head} ({len(versions)} events)")
        # Both events of one frame survive or neither does.
        ids = {change["key"]["id"] for change in changes}
        torn = [i for i in ids if i.startswith(f"{NET}:c") and not i.endswith("b") and f"{i}b" not in ids]
        if torn:
            fail(f"crash: torn frames {torn[:3]}")
        if early_cursor:
            status, page = client.sync(early_cursor, 500)
            if status != 200 or page["reset"] or [seq_of(c) for c in page["changes"]][:1] != [6]:
                fail(f"crash: the pre-crash cursor does not continue at 6 ({status})")
        status, result = client.append(address, [event("after", now + 1000)])
        if status != 200 or int(result["head"]) != head + 1:
            fail(f"crash: next append took {result} after head {head}")
        print(f"native crash: {head} events survived ({appended[0]} acknowledged before SIGKILL), "
              f"contiguous; next append {head + 1}", flush=True)
        if head < 5 + appended[0]:
            fail(f"crash: acknowledged appends lost ({head} < {5 + appended[0]})")
    finally:
        server.stop()


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    changes = json.loads((Path(__file__).parent / "fixtures/strict_sync_order_cases.json").read_text())
    for case in fixture["cases"]:
        case.update(changes.get(case["name"], {}))
    with tempfile.TemporaryDirectory(prefix="fortis-sync-edges-") as root:
        root = Path(root)
        upstream = replay.Upstream(str(root), replay.Table({}, []))
        try:
            totals = replay_fixture(binary, root, upstream, fixture)
            if os.environ.get("SYNC_EDGES_SKIP_NATIVE") != "1":
                native_concurrency(binary, root, upstream)
                native_crash(binary, root, upstream)
        finally:
            upstream.close()
    print(f"{FIXTURE}: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("sync edges: all checks passed")


if __name__ == "__main__":
    main()
