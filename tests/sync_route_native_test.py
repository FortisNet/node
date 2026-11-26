#!/usr/bin/env python3
"""Native-only checks of POST /v1/sync that the TypeScript replay
(tests/sync_route_test.py) cannot express:

  1. Gating: the normal binary registers the route only with
     FORTIS_NATIVE_ADDRESS_CANARY=1 (404 without, 401 unauthenticated with).
  2. Paging at scale: 1,200 events over three accounts (device A watches two,
     device B the third) page out at limit 500 in strict global sequence
     order, JSON and protobuf, with hasMore/cursor continuity, no duplicate,
     no gap and no other device's account. Page latency is reported.
  3. Concurrency: eight pollers page while a writer appends; every answer is
     200, ordered, within the poller's scope; the final read sees every
     event exactly once.
  4. Crash: a writer appends one event per frame while the process is
     SIGKILLed; after restart the watched stream is exactly 1..head (no event
     without its head advance and no head without its event) and the next
     append continues at head + 1.
  5. Bounds and memory: 100 limit-500 pages report RSS growth (bounded); a
     page whose response would pass the 2 MiB native bound ends early with
     hasMore, and paging on still delivers every event once, in order.

Usage: sync_route_native_test.py <normal binary> <address test binary>
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import address_routes_native_test as native
import address_routes_test as replay
import discovery_route_harness as harness

NOW = 1767323045000
NET = "eth-mainnet"
TOKEN_A = "fintel_dev_" + "A" * 43
TOKEN_B = "fintel_dev_" + "B" * 43
ACCOUNTS = [f"0x{'a' * 39}{index}" for index in range(1, 4)]
MAX_PAGE_RSS_GROWTH_KIB = 64 * 1024

# A transfer row's app JSON, as the writer journals it (about 1.2 KB).
FIELDS = {
    "id": None, "type": "receive", "status": "confirmed", "direction": "incoming",
    "timestamp": "2025-12-30T03:04:05.000Z", "chainId": NET, "family": "evm", "hash": "0x" + "a1" * 32,
    "blockNumber": 23899000, "nonce": None, "confirmations": 1, "fromAddress": "0x" + "9" * 40,
    "toAddress": "0x" + "5" * 40, "asset": {"chainId": NET, "symbol": "ETH", "name": "ETH", "decimals": 18,
                                            "contractAddress": None},
    "amount": {"raw": "2000000000000000000", "decimals": 18}, "toAsset": None, "toAmount": None,
    "destinationChainId": None, "destinationHash": None, "fee": {"raw": "420000000000000", "decimals": 18},
    "feeSymbol": "ETH", "fiatValue": None, "fiatCurrency": None, "fiatValueSource": None,
    "fiatValueSourceId": None, "fiatValueAsOf": None, "memo": None, "error": None, "metadata": {"note": "x" * 300},
}


def varint(data, position):
    value = shift = 0
    while True:
        byte = data[position]
        position += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if not byte & 0x80:
            return value, position


def proto_fields(data):
    """Top-level (field, value) pairs of a message (varint and bytes only)."""
    position, out = 0, []
    while position < len(data):
        tag, position = varint(data, position)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            value, position = varint(data, position)
        elif wire == 2:
            length, position = varint(data, position)
            value = data[position:position + length]
            position += length
        else:
            raise ValueError(f"wire type {wire}")
        out.append((field, value))
    return out


def decode_page(payload, protobuf):
    """(cursor, hasMore, reset, [(version, address)])."""
    if not protobuf:
        page = json.loads(payload)
        return page["cursor"], page["hasMore"], page["reset"], [
            (change["version"], change["key"]["address"]) for change in page["changes"]]
    cursor, has_more, reset, changes = "", False, False, []
    for field, value in proto_fields(payload):
        if field == 2:
            cursor = value.decode()
        elif field == 3:
            has_more = bool(value)
        elif field == 4:
            reset = bool(value)
        elif field == 7:
            change = dict(proto_fields(value))
            # key Struct: map entries {1: name, 2: Value{3: string}}.
            key = {}
            for entry_field, entry in proto_fields(change[2]):
                pair = dict(proto_fields(entry))
                key[pair[1].decode()] = dict(proto_fields(pair[2])).get(3, b"").decode()
            changes.append((change[4].decode(), key.get("address")))
    return cursor, has_more, reset, changes


class Client:
    def __init__(self, server):
        self.server = server

    def append(self, address, count, start, size=None):
        events = []
        for index in range(count):
            record = f"{NET}:0x{start + index:064x}"
            fields = dict(FIELDS, id=record)
            if size:
                fields["metadata"] = {"note": "y" * size}
            events.append({"entityKey": {"id": record}, "operation": "upsert", "fields": fields,
                           "changedAt": NOW})
        status, _, body = self.server.request("POST", "/probe/test-sync-append",
                                              body=json.dumps({"network": NET, "address": address,
                                                               "events": events, "rollback": False}).encode(),
                                              headers={"Content-Type": "application/json"}, timeout=60)
        if status != 200:
            raise RuntimeError(f"append {status} {body[:200]!r}")
        return json.loads(body)["head"]

    def watch(self, token, address):
        body = json.dumps({"topic": f"address:{NET}:{address}", "account": {"network": NET, "address": address},
                           "expiresAt": NOW + 3600 * 1000}).encode()
        status, _, payload = self.server.request("POST", "/probe/test-sync-watch", body=body,
                                                 headers={"Content-Type": "application/json",
                                                          "Authorization": "Bearer " + token})
        if status != 200:
            raise RuntimeError(f"watch {status} {payload[:200]!r}")

    def sync(self, token, cursor, limit=500, protobuf=False):
        headers = {"Authorization": "Bearer " + token, "Content-Type": "application/json",
                   "Accept": "application/protobuf" if protobuf else "application/json"}
        body = json.dumps({"cursor": cursor, "limit": limit}).encode()
        return self.server.request("POST", "/v1/sync", body=body, headers=headers, timeout=60)

    def read_all(self, token, protobuf=False, limit=500):
        """Pages from sequence zero to the head; returns (changes, pages, seconds)."""
        cursor = f"s2_{NOW:013d}_{'0' * 20}"
        changes, pages, seconds = [], 0, []
        while True:
            started = time.monotonic()
            status, _, payload = self.sync(token, cursor, limit, protobuf)
            seconds.append(time.monotonic() - started)
            if status != 200:
                raise RuntimeError(f"sync {status} {payload[:200]!r}")
            cursor, has_more, reset, page = decode_page(payload, protobuf)
            if reset:
                raise RuntimeError("unexpected reset")
            if len(page) > limit:
                raise RuntimeError(f"page of {len(page)} > {limit}")
            changes.extend(page)
            pages += 1
            if not has_more:
                return changes, pages, seconds, cursor


def ordered(changes):
    versions = [int(version[3:]) for version, _ in changes]
    return all(a < b for a, b in zip(versions, versions[1:]))


def check_gating(normal, root, ca, failures):
    for canary, expected in (("0", 404), ("1", 401)):
        kv = Path(root) / f"kv-gating-{canary}"
        kv.mkdir()
        env = {"FORTIS_NATIVE_ADDRESS_CANARY": canary}
        if canary == "1":
            env["FORTIS_CA_BUNDLE_PATH"] = str(ca)
        server = native.NormalServer(normal, kv, env)
        try:
            status, _, body = server.request("POST", "/v1/sync", body=b"{}",
                                             headers={"Content-Type": "application/json"})
            if status != expected:
                failures.append(f"gating canary={canary}: {status} != {expected} {body[:120]!r}")
        finally:
            server.stop()


def check_paging(client, server, failures):
    for account in ACCOUNTS:
        client.append(account, 1, 0)
    client.watch(TOKEN_A, ACCOUNTS[0])
    client.watch(TOKEN_A, ACCOUNTS[1])
    client.watch(TOKEN_B, ACCOUNTS[2])
    start = 1000
    for _ in range(4):
        for account in ACCOUNTS:
            client.append(account, 100, start)
            start += 100
    expected_a = 2 + 2 * 400
    expected_b = 1 + 400
    for protobuf in (False, True):
        label = "protobuf" if protobuf else "json"
        changes, pages, seconds, cursor = client.read_all(TOKEN_A, protobuf)
        print(f"paging {label}: {len(changes)} events in {pages} pages, slowest page "
              f"{max(seconds) * 1000:.1f} ms, mean {sum(seconds) / len(seconds) * 1000:.1f} ms")
        if len(changes) != expected_a or len({v for v, _ in changes}) != expected_a:
            failures.append(f"paging {label}: {len(changes)} events != {expected_a}")
        if not ordered(changes):
            failures.append(f"paging {label}: versions not strictly increasing")
        if {address for _, address in changes} != set(ACCOUNTS[:2]):
            failures.append(f"paging {label}: scope leak {sorted({a for _, a in changes})}")
        if pages != 2:
            failures.append(f"paging {label}: {pages} pages != 2")
        if not cursor.endswith(f"_{1203:020d}"):
            failures.append(f"paging {label}: final cursor {cursor}")
    changes, _, _, _ = client.read_all(TOKEN_B)
    if len(changes) != expected_b or {a for _, a in changes} != {ACCOUNTS[2]} or not ordered(changes):
        failures.append(f"device B: {len(changes)} events, scope {sorted({a for _, a in changes})}")
    # Memory: 100 full pages.
    before = native.rss_kib(server.process.pid)
    for _ in range(50):
        client.read_all(TOKEN_A)
    after = native.rss_kib(server.process.pid)
    print(f"memory: 100 limit-500 pages rss {before} KiB -> {after} KiB (+{after - before} KiB)")
    if after - before > MAX_PAGE_RSS_GROWTH_KIB:
        failures.append(f"page rss growth {after - before} KiB > {MAX_PAGE_RSS_GROWTH_KIB} KiB")
    return expected_a, expected_b


def check_concurrency(client, failures, expected_a, expected_b):
    stop = threading.Event()
    errors = []
    lock = threading.Lock()
    written = {"a": 0, "b": 0}

    def poller(token, scope):
        while not stop.is_set():
            try:
                status, _, payload = client.sync(token, f"s2_{NOW:013d}_{'0' * 20}")
                if status != 200:
                    raise RuntimeError(f"status {status} {payload[:120]!r}")
                _, _, _, page = decode_page(payload, False)
                if not ordered(page) or not {a for _, a in page} <= scope:
                    raise RuntimeError("unordered page or scope leak")
            except Exception as error:  # noqa: BLE001 - collected for the report
                with lock:
                    errors.append(str(error))
                return

    def writer():
        start = 50000
        while not stop.is_set():
            client.append(ACCOUNTS[0], 10, start)
            client.append(ACCOUNTS[2], 10, start)
            start += 10
            written["a"] += 10
            written["b"] += 10

    threads = [threading.Thread(target=poller, args=(TOKEN_A, set(ACCOUNTS[:2]))) for _ in range(6)]
    threads += [threading.Thread(target=poller, args=(TOKEN_B, {ACCOUNTS[2]})) for _ in range(2)]
    threads.append(threading.Thread(target=writer))
    for thread in threads:
        thread.start()
    time.sleep(3)
    stop.set()
    for thread in threads:
        thread.join(timeout=60)
    if errors:
        failures.append(f"concurrency: {errors[:3]}")
    changes, _, _, _ = client.read_all(TOKEN_A)
    if len(changes) != expected_a + written["a"] or not ordered(changes):
        failures.append(f"concurrency A: {len(changes)} != {expected_a + written['a']}")
    changes, _, _, _ = client.read_all(TOKEN_B)
    if len(changes) != expected_b + written["b"] or not ordered(changes):
        failures.append(f"concurrency B: {len(changes)} != {expected_b + written['b']}")
    print(f"concurrency: 8 pollers, {written['a'] + written['b']} events appended meanwhile")


def check_bound(client, clock, failures):
    # 500 events with 40 KB fields: about 20 MB, past the 2 MiB response
    # bound. Pages end early with hasMore and page on to every event.
    account = "0x" + "c" * 40
    head = client.append(account, 1, 899999)
    for batch in range(50):
        client.append(account, 10, 900000 + batch * 10, size=40000)
    client.watch(TOKEN_A, account)
    # A direct watch write is not a topic write: past the four-minute scope
    # cache the device sees the account.
    harness.set_clock(clock, NOW + 5 * 60 * 1000)
    for protobuf in (False, True):
        cursor = f"s2_{NOW:013d}_{head}"
        changes, sizes = [], []
        while True:
            status, _, body = client.sync(TOKEN_A, cursor, protobuf=protobuf)
            if status != 200:
                failures.append(f"response bound: {status} {body[:160]!r}")
                return
            sizes.append(len(body))
            cursor, has_more, _, page = decode_page(body, protobuf)
            changes.extend(page)
            if not has_more:
                break
        label = "protobuf" if protobuf else "json"
        print(f"response bound {label}: 500 events of ~40 KB in {len(sizes)} pages, largest {max(sizes)} bytes")
        if len(changes) != 500 or len({v for v, _ in changes}) != 500 or not ordered(changes):
            failures.append(f"response bound {label}: {len(changes)} events")
        if max(sizes) > 2 * 1024 * 1024 or len(sizes) < 10:
            failures.append(f"response bound {label}: page sizes {sizes}")


def check_crash(binary, root, upstream, failures):
    kv = Path(root) / "kv-crash"
    kv.mkdir()
    clock = Path(root) / "clock-crash"
    harness.set_clock(clock, NOW)
    server = replay.Server(binary, kv, clock, upstream, {})
    client = Client(server)
    client.append(ACCOUNTS[0], 1, 0)
    client.watch(TOKEN_A, ACCOUNTS[0])
    stop = threading.Event()

    def writer():
        index = 1
        while not stop.is_set():
            try:
                client.append(ACCOUNTS[0], 1, index)
            except Exception:  # noqa: BLE001 - the process is killed under it
                return
            index += 1

    thread = threading.Thread(target=writer)
    thread.start()
    time.sleep(1.0)
    server.process.send_signal(signal.SIGKILL)
    server.process.wait()
    stop.set()
    thread.join(timeout=30)
    server = replay.Server(binary, kv, clock, upstream, {})
    client = Client(server)
    try:
        changes, _, _, cursor = client.read_all(TOKEN_A)
        head = int(cursor.rsplit("_", 1)[1])
        versions = [int(v[3:]) for v, _ in changes]
        if versions != list(range(1, head + 1)):
            failures.append(f"crash: stream {versions[:3]}…{versions[-3:]} != 1..{head}")
        after = client.append(ACCOUNTS[0], 1, 10 ** 6)
        if int(after) != head + 1:
            failures.append(f"crash: next append {after} != {head + 1}")
        print(f"crash: SIGKILL after {head} committed appends; stream 1..{head} intact, next {after}")
    finally:
        server.stop()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    normal, binary = sys.argv[1], sys.argv[2]
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-sync-native-") as root:
        table = replay.Table({}, [])
        upstream = replay.Upstream(root, table)
        try:
            check_gating(normal, root, upstream.ca, failures)
            kv = Path(root) / "kv"
            kv.mkdir()
            clock = Path(root) / "clock"
            harness.set_clock(clock, NOW)
            server = replay.Server(binary, kv, clock, upstream, {})
            client = Client(server)
            try:
                expected_a, expected_b = check_paging(client, server, failures)
                check_concurrency(client, failures, expected_a, expected_b)
                check_bound(client, clock, failures)
                if server.process.poll() is not None:
                    failures.append("server exited")
            finally:
                server.stop()
            check_crash(binary, root, upstream, failures)
        finally:
            upstream.close()
    if failures:
        print(f"sync native checks: {len(failures)} failure(s)")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print("sync native checks passed")


if __name__ == "__main__":
    main()
