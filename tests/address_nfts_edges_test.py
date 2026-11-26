#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/address/{address}/nfts
(R15) against tests/corpus/address_nfts_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets.

The harness is R13's (tests/address_balances_edges_test.py) with R14's timed
table: the test binary (build/fortis_node_address_route_test) sends
every provider host to one loopback HTTPS upstream that answers, per case,
the exact bytes TypeScript was served in the order TypeScript asked for them
(a host the case marked down answers 503). Each case's frozen instant is
written to the shared clock file first and the native provider lanes are
paced by monotonic time. Each fixture group is a fresh process with its own
KV directory; restart-b reuses restart-a's after a SIGKILL.

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, and the
multiset of provider requests (collected after /probe/test-address-idle); no
request may reach an image host. Concurrent cases compare every response and
the combined provider requests. `state` records compare, through POST
/probe/test-address-state, the device's watches and the account rows, and
through POST /probe/test-nft-state the scan record, every live item record
(accountId as "<account>") and the media records, exactly; then each expiry
TypeScript recorded is probed one millisecond before and at the instant, so
the seven-day TTLs of item, scan and media records are checked to the
millisecond. No provider request may carry a client header or credential,
and no device token, id or provider key may reach a response or process
output (the provider keys only their own provider's URL).

Native-only bounded rules (not frozen by the oracle):
  - `bounded-inventory`: a 2,000-item inventory, then a rescan returning
    2,000 other items (4,000 records, half gone): every read answers exactly
    the live items in key order; then a 4,000-entry page (about 0.9 MiB, over
    the 512 KiB provider-execution body bound): the scan fails and the
    stored gallery is served stale;
  - `bounded-workers`: eight distinct cold accounts at once against a slow
    upstream all answer 200 with their own gallery, one provider request
    each, never more than two in flight;
  - the production composition: the route answers 404 unless
    FORTIS_NATIVE_ADDRESS_CANARY=1, then 401/403 before any facet work.

Asserted differences (documented beside handle_nfts):
  - router: other methods and spellings are 404 in both (status only); the
    routing 414 and Node's "Client Error" 400 carry the native security
    headers; a malformed path escape is the parser's {"error":"BAD_REQUEST"}
    (Fastify FST_ERR_BAD_URL); a raw '#' is refused by the request parser
    (TypeScript drops the fragment and scans), and so is a raw byte that
    leaves the decoded path invalid UTF-8 (Node: "Client Error");
  - a decoded NUL in the address: 400 without an account (TypeScript touches
    an account for it and answers a failed scan, 200 stale);
  - an item key over the native KV's 1,024-byte bound fails the scan (stale,
    the gallery kept) where TypeScript stores it;
  - lastError: TypeScript stores String(err); native stores its own failure
    text. RangeError, URIError and LMDB key-size failures match exactly;
    for any other failure only its presence is compared (no response shows
    it).

Usage: address_nfts_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import os
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import address_balances_edges_test as edges  # noqa: E402
import address_insight_edges_test as insight  # noqa: E402
import address_routes_test as routes  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "address_nfts_edges.json"
check = edges.check
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path"}


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    import strict_nft_expectations
    return strict_nft_expectations.apply(json.loads(data))


def nul_address(case, actual, observed):
    """A decoded NUL in the address: TypeScript touches an account for it and
    answers its failed scan (200 stale); native refuses it (400, no account,
    no provider request either way)."""
    return check(case["status"] == 200 and json.loads(case["responseText"])["stale"] is True and
                 not case["upstream"] and actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def native_key_bound(case, actual, observed):
    """An item key over the native KV's 1,024-byte bound (an 1,100-digit token
    id): TypeScript stores the gallery; native fails the scan, so this first
    read is stale and empty, after the same provider request."""
    document = json.loads(actual[2]) if actual[0] == 200 else {}
    expected = json.loads(case["responseText"])
    return check(actual[0] == 200 and expected["stale"] is False and len(expected["items"]) == 2 and
                 document.get("stale") is True and document.get("items") == [] and
                 document.get("cache") == {"hit": True, "timestamp": None} and
                 edges.compare_upstream(case["name"], case, observed, []),
                 f"{case['name']}: native {actual[0]} {actual[2][:160]!r}")


# lmdb-msgpack: these read records that hold a lone surrogate,
# which are stored as written now (compat_expectations.surrogate_text).
SURROGATE_CASES = {"names", "surrogate-name", "surrogate-token-id"}
SURROGATE_STATES = {"long-surrogate-state", "surrogate-token-state"}


def stored_as_written(case, actual, observed):
    """The answer equals the oracle's but for lone surrogates and the media
    keys derived from them, and holds no msgpack triple U+FFFD."""
    want = compat_expectations.surrogate_text(case["responseText"])
    have = compat_expectations.surrogate_text(actual[2])
    ok = check(actual[0] == case["status"] and want == have and "\ufffd\ufffd\ufffd" not in actual[2].decode(
        "utf-8", "surrogatepass"), f"{case['name']}: differs beyond the stored-as-written surrogates\n"
                                   f"  expected {want[:600]!r}\n  actual   {have[:600]!r}")
    for name in edges.HEADERS:
        if name in ("content-length", "etag", "x-rate-limit-remaining"):
            continue
        ok = check(actual[1].get(name) == case["headers"].get(name),
                   f"{case['name']}: header {name} {actual[1].get(name)!r} != {case['headers'].get(name)!r}") and ok
    return edges.compare_upstream(case["name"], case, observed, []) and ok


ASSERTED = {
    **{name: stored_as_written for name in SURROGATE_CASES},
    "token-id-over-native-key-bound": native_key_bound,
    "address-malformed-escape": edges.bad_url, "address-escaped-nul": nul_address,
    "fragment": insight.fragment_refusal, "raw-latin1-address": insight.invalid_utf8_path,
}


def exact_error(text):
    return text is None or text.startswith(("RangeError: ", "URIError: "))


# lmdb-msgpack: LMDB's key bound is gone; the native bound's text.
LMDB_KEY_ERROR = "Error: Key size is larger than the maximum key size (1978)"
NATIVE_KEY_ERROR = "Error: key exceeds the native KV key bound (1024)"


def nft_probe(server, case, now=None):
    accounts = case["state"]["accounts"]
    body = {"accounts": accounts, "mediaKeys": [m["key"] for m in case["media"]]}
    if now is not None:
        body["now"] = now
    status, _, payload = server.request("POST", "/probe/test-nft-state", body=json.dumps(body).encode(),
                                        headers={"Content-Type": "application/json"})
    if not check(status == 200, f"{case['name']}: nft probe {status} {payload[:200]!r}"):
        return None
    return json.loads(payload)


def compare_nft_state(h, case, got):
    name = case["name"]
    ok = True
    for want, have in zip(case["nft"], got["nft"]):
        label = f"{name}: {want['network']}/{want['address'][:24]}"
        want_scan = want["scan"]["value"] if want["scan"] else None
        have_scan = have["scan"]
        if any(ord(c) < 0x20 or c == "\x7f" for c in want["address"]):
            # opaque-params: the router refused that path, so no
            # scan ran for it.
            want_scan, want = None, dict(want, items=[])
        if want_scan is not None and have_scan is not None:
            for secret in h.fixture["providerKeys"]:
                ok = check(secret not in str(have_scan.get("lastError")), f"{label}: lastError holds a key") and ok
            ts_error, native_error = want_scan.get("lastError"), have_scan.get("lastError")
            if ts_error == LMDB_KEY_ERROR:
                want_scan = dict(want_scan, lastError=NATIVE_KEY_ERROR)
            elif not exact_error(ts_error) and native_error is not None:
                have_scan = dict(have_scan, lastError=ts_error)
        ok = check(have_scan == want_scan, f"{label}: scan record\n  expected {want_scan}\n  actual   {have_scan}") \
            and ok
        want_items = [{"key": i["key"], "value": i["value"]} for i in want["items"]]
        if name in SURROGATE_STATES and want_items != have["items"]:
            surrogate = compat_expectations.surrogate_text
            if surrogate(json.dumps(want_items, ensure_ascii=False)) == \
                    surrogate(json.dumps(have["items"], ensure_ascii=False)):
                want_items = have["items"]
        if have["items"] != want_items:
            index = next((i for i, (x, y) in enumerate(zip(want_items, have["items"])) if x != y),
                         min(len(want_items), len(have["items"])))
            ok = check(False, f"{label}: item records ({len(want_items)} expected, {len(have['items'])} stored), "
                              f"first difference at {index}\n  expected {json.dumps(want_items[index:index + 1])[:900]}"
                              f"\n  actual   {json.dumps(have['items'][index:index + 1])[:900]}")
    ok = check(len(case["nft"]) == len(got["nft"]), f"{name}: account count") and ok
    for want, have in zip(case["media"], got["media"]):
        expected = {k: v for k, v in want.items() if k != "expiresAt"}
        if name in SURROGATE_STATES and expected != have:
            surrogate = compat_expectations.surrogate_text
            if surrogate(json.dumps(expected, ensure_ascii=False)) == surrogate(json.dumps(have, ensure_ascii=False)):
                continue
        ok = check(have == expected, f"{name}: media {want['key']}\n  expected {expected}\n  actual   {have}") and ok
    return ok


def visible(case, at):
    """What TypeScript's store holds at `at` (ms): item keys, scans, media
    (minus the accounts of a path value the router now refuses:
    opaque-params)."""
    refused = lambda row: any(ord(c) < 0x20 or c == "\x7f" for c in row["address"])  # noqa: E731
    items = [[i["key"] for i in row["items"] if not refused(row) and (i["expiresAt"] is None or i["expiresAt"] > at)]
             for row in case["nft"]]
    scans = [bool(row["scan"]) and not refused(row) and (row["scan"]["expiresAt"] is None or
                                                        row["scan"]["expiresAt"] > at)
             for row in case["nft"]]
    media = [("value" in m) and (m["expiresAt"] is None or m["expiresAt"] > at) for m in case["media"]]
    return items, scans, media


def observed_visible(got):
    return ([[i["key"] for i in row["items"]] for row in got["nft"]], [row["scan"] is not None for row in got["nft"]],
            ["value" in m for m in got["media"]])


def replay_nft_state(h, server, case):
    if not edges.replay_state(h, server, case):
        return False
    got = nft_probe(server, case)
    if got is None or not compare_nft_state(h, case, got):
        return False
    # Every recorded expiry, one millisecond before and at the instant.
    instants = sorted({e for row in case["nft"] for e in
                       [row["scan"]["expiresAt"] if row["scan"] else None] + [i["expiresAt"] for i in row["items"]]
                       if e is not None} | {m["expiresAt"] for m in case["media"] if m.get("expiresAt")})
    ok = True
    for instant in instants:
        for at in (instant - 1, instant):
            got = nft_probe(server, case, at)
            ok = got is not None and check(observed_visible(got) == visible(case, at),
                                           f"{case['name']}: records visible at {at} (expiry {instant})\n"
                                           f"  expected {visible(case, at)}\n  actual   {observed_visible(got)}") and ok
    return ok


def compare_case(h, case, actual, observed):
    name = case["name"]
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(actual[0], actual[1], actual[2], observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        return "asserted" if check(case["status"] == 404 and actual[0] == 404 and not observed,
                                   f"{name}: status {actual[0]} != 404") else "failed"
    rule = ASSERTED.get(name)
    if rule:
        return "asserted" if rule(case, actual, observed) else "failed"
    if case["status"] in (414, 431) or name.startswith("raw-utf8"):
        ok = edges.compare(name, case, actual, baseline=True) and \
            edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    ok = edges.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
    return "matched" if ok else "failed"


class QuotaPacer:
    """The native device quota counts wall-clock minutes (TypeScript's the
    frozen case Date): before each charged request, wait for the next minute
    when the device's budget (premium 1,200, development 240; 20 per read)
    would run out."""
    LIMITS = {"A": 1200, "B": 1200}

    def __init__(self):
        self.used = collections.Counter()

    def charge(self, case, count=1):
        device = case.get("device")
        if not device or case["group"] == "quota":
            return
        limit = self.LIMITS.get(device, 240)
        minute = int(time.time() // 60)
        if self.used[(device, minute)] + 20 * count > limit:
            time.sleep(60 - time.time() % 60 + 0.2)
            minute = int(time.time() // 60)
        self.used[(device, minute)] += 20 * count


PACER = QuotaPacer()


def provider_keys_contained(h, name, observed, payloads):
    """Each provider key only in its own provider's requests; never in a
    response."""
    alchemy, helius = h.provider_keys
    ok = True
    for method, host, target, body in observed:
        text = f"{target} {body}"
        if alchemy in text:
            ok = check(host.endswith(".g.alchemy.com"), f"{name}: the Alchemy key went to {host}") and ok
        if helius in text:
            ok = check(host == "mainnet.helius-rpc.com", f"{name}: the Helius key went to {host}") and ok
    for payload in payloads:
        for key in h.provider_keys:
            ok = check(key.encode() not in payload, f"{name}: a provider key reached a response") and ok
    return ok


def no_image_host(name, observed):
    return check(not any("img.example" in host or "127.0.0.1" == host for _, host, _, _ in observed),
                 f"{name}: a read contacted an image host")


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        return "matched" if replay_nft_state(h, server, case) else "failed"
    lanes.reserve(case)
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case)
    before = len(h.upstream.calls())
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    if "concurrent" in case:
        kinds = case["concurrent"]
        results = [None] * len(kinds)
        barrier = threading.Barrier(len(kinds))

        def one(index, entry):
            barrier.wait()
            try:
                results[index] = catalog.raw(server.port, "HEAD" if entry["kind"] == "head" else "GET",
                                             entry.get("target") or target, edges.headers_of(case, entry["kind"]))
            except (OSError, IndexError, ValueError) as error:
                results[index] = (0, {}, repr(error).encode())

        PACER.charge(case, len(kinds))
        threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
        edges.repay(lanes, case, observed)
        ok = no_image_host(name, observed)
        ok = provider_keys_contained(h, name, observed, [r[2] for r in results]) and ok
        for index, (record, actual) in enumerate(zip(case["responses"], results)):
            h.no_secret(actual[2], name)
            ok = edges.compare(f"{name}[{index}:{kinds[index]['kind']}]", record, actual) and ok
        ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
        return "matched" if ok else "failed"
    PACER.charge(case)
    try:
        actual = catalog.raw(server.port, case["method"], target, edges.headers_of(case))
    except OSError as error:
        check(False, f"{name}: {error!r} (TypeScript {case['status']})")
        edges.settle(server, h.upstream, before, len(case["upstream"]))
        return "failed"
    h.no_secret(actual[2], name)
    observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
    edges.repay(lanes, case, observed)
    no_image_host(name, observed)
    provider_keys_contained(h, name, observed, [actual[2]])
    return compare_case(h, case, actual, observed)


# ---- native-only bounded rules ---------------------------------------------------------------------


def evm_inventory(owner, token_ids):
    return {"ownedNfts": [{"tokenId": t, "tokenType": "ERC721", "name": f"Bound {t}",
                           "contract": {"address": "0x" + "cd" * 20, "name": "Bound"},
                           "image": {"cachedUrl": f"https://img.example/bound/{t}.png"}} for t in token_ids]}


def get_json(server, path):
    PACER.charge({"device": "A", "group": "bounded"})
    status, headers, payload = catalog.raw(server.port, "GET", path, {"authorization": "Bearer " + catalog.token("A")})
    return status, (json.loads(payload) if status == 200 else payload)


def bounded_inventory(h, server, fixture):
    """2,000 items, then 2,000 others (4,000 records, the first 2,000 gone),
    then a page over the provider body bound: stale, the gallery kept."""
    owner = "0x" + "bd" * 20
    path = f"/v1/networks/eth-mainnet/address/{owner}/nfts"
    size = int(os.environ.get("R15_BOUND_ITEMS", "2000"))
    inventory = {"ids": [f"{i:05d}" for i in range(size)]}

    def answer(method, host, target, body):
        if host == "eth-mainnet.g.alchemy.com" and "getNFTsForOwner" in target:
            return 200, json.dumps(evm_inventory(owner, inventory["ids"])).encode(), 5, True
        return 404, b'{"error":"unexpected request"}', 5, True

    start = fixture["now"] + 200 * 86400000
    h.table.override = answer
    try:
        harness.set_clock(h.clock, start)
        status, first = get_json(server, path)
        ok = check(status == 200 and [i["tokenId"] for i in first["items"]] == inventory["ids"] and
                   first["stale"] is False, f"bounded-inventory first: {status} {str(first)[:200]}")
        inventory["ids"] = [f"{i:05d}" for i in range(size, 2 * size)]
        harness.set_clock(h.clock, start + 11 * 60000)
        status, second = get_json(server, path)
        ok = check(status == 200 and [i["tokenId"] for i in second["items"]] == inventory["ids"] and
                   second["stale"] is False and len(second["items"]) == size,
                   f"bounded-inventory rescan: {status} {str(second)[:200]}") and ok
        harness.set_clock(h.clock, start + 12 * 60000)
        status, third = get_json(server, path)
        ok = check(status == 200 and third == second, f"bounded-inventory read: {status}") and ok
        # A provider page over the 512 KiB provider-execution body bound
        # (4,000 entries, about 0.9 MiB): the scan fails, the gallery stays.
        inventory["ids"] = [f"{i:05d}" for i in range(4 * size, 6 * size)]
        harness.set_clock(h.clock, start + 23 * 60000)
        status, over = get_json(server, path)
        ok = check(status == 200 and over["stale"] is True and over["items"] == second["items"],
                   f"bounded-inventory over the body bound: {status} {str(over)[:200]}") and ok
    finally:
        h.table.override = None
    print(f"bounded-inventory: {size} then {size} other items, {2 * size} records: {'ok' if ok else 'FAILED'}",
          flush=True)
    return ok


def bounded_workers(h, server, fixture):
    owners = [f"0x{index:040x}" for index in range(0x7000, 0x7008)]

    def answer(method, host, target, body):
        owner = target.split("owner=")[1].split("&")[0]
        return 200, json.dumps(evm_inventory(owner, [owner[-4:]])).encode(), 400, True

    harness.set_clock(h.clock, fixture["now"] + 210 * 86400000)
    PACER.charge({"device": "A", "group": "bounded"}, len(owners))
    time.sleep(1.2)
    h.table.override = answer
    h.table.spans = []
    results = [None] * len(owners)
    barrier = threading.Barrier(len(owners))

    def one(index):
        barrier.wait()
        results[index] = catalog.raw(server.port, "GET", f"/v1/networks/eth-mainnet/address/{owners[index]}/nfts",
                                     {"authorization": "Bearer " + catalog.token("A")})

    threads = [threading.Thread(target=one, args=(i,)) for i in range(len(owners))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    h.table.override = None
    ok = True
    for index, result in enumerate(results):
        document = json.loads(result[2]) if result and result[0] == 200 else {}
        ok = check([i.get("tokenId") for i in document.get("items", [])] == [owners[index][-4:]],
                   f"bounded-workers[{index}]: {result[0] if result else None} {result[2][:160] if result else b''!r}") \
            and ok
    events = sorted([(start, 1) for start, _ in h.table.spans] + [(end, -1) for _, end in h.table.spans])
    peak = current = 0
    for _, delta in events:
        current += delta
        peak = max(peak, current)
    ok = check(len(h.table.spans) == len(owners) and peak <= 2,
               f"bounded-workers: {len(h.table.spans)} provider requests, peak in flight {peak} (bound 2)") and ok
    print(f"bounded-workers: {len(owners)} cold accounts, {len(h.table.spans)} provider requests, peak in flight "
          f"{peak}", flush=True)
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R15_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="address-nfts-") as root:
        root = Path(root)
        h = edges.Harness(binary, root, fixture)
        h.provider_keys = list(fixture["providerKeys"])
        h.table = insight.TimedTable(fixture["cases"])
        h.upstream.table = h.table
        try:
            for name, cases in by_group.items():
                bounded_only = name == "main" and "bounded" in only
                if only and name not in only and not bounded_only and \
                        not (name.startswith("restart") and "restart" in only):
                    continue
                group = groups[name]
                server = h.server(group["kv"], group["env"], group["start"])
                lanes = routes.Lanes()
                results = collections.Counter()
                try:
                    if name == "quota":
                        catalog.wait_for_fresh_minute(25)
                        for _ in range(12):
                            status, _, _ = catalog.raw(server.port, "GET", "/v1/networks/nowhere/address/"
                                                       + fixture["holders"]["GALLERY"] + "/nfts",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in ([] if bounded_only and name not in only else cases):
                        results[replay_case(h, server, case, lanes)] += 1
                        if os.environ.get("R15_FAIL_FAST") and edges.FAILURES:
                            raise SystemExit(1)
                    if name == "main" and (not only or bounded_only):
                        results["matched" if bounded_inventory(h, server, fixture) else "failed"] += 1
                        results["matched" if bounded_workers(h, server, fixture) else "failed"] += 1
                finally:
                    if name == "restart-a":
                        server.process.send_signal(signal.SIGKILL)
                        server.process.wait()
                    else:
                        server.stop()
                    output = server.process.stdout.read() + server.process.stderr.read()
                    h.no_secret(output, f"{name} output")
                    for key in h.provider_keys:
                        check(key.encode() not in output, f"{name} output: a provider key was logged")
                totals.update(results)
                print(f"{name}: {results['matched']} matched, {results['asserted']} asserted differences, "
                      f"{results['failed']} failed", flush=True)
        finally:
            h.upstream.close()
        if h.table.extra:
            print(f"requests TypeScript never made: {sorted(set(h.table.extra))[:10]}")
        if len(sys.argv) == 3:
            production(sys.argv[2], root, fixture)
    print(f"address nfts: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    print("address nfts edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/address/" + fixture["holders"]["GALLERY"] + "/nfts"
    for value in (None, "0"):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ADDRESS_CANARY"] = value
        server = catalog.Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            status, _, _ = catalog.raw(server.port, "GET", path, {"authorization": "Bearer " + catalog.token("a")})
            check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            server.stop()
    server = catalog.Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ADDRESS_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca)})
    try:
        status, headers, body = catalog.raw(server.port, "GET", path, {})
        check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
              f"production no-auth: {status} {body!r}")
        status, _, body = catalog.raw(server.port, "GET", path, {"origin": "https://evil.example"})
        check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/eth-mainnet/address//nfts", {})
        # opaque-params: the router's 404 before any hook.
        check(status == 404, f"production empty address without a bearer: {status} {body!r}")
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
