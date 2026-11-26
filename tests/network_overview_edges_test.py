#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/overview (R18) against
tests/corpus/network_overview_edges.json, which
the earlier TypeScript implementation's tests recorded
from the real Fastify app over real sockets. The fixture's SHA-256 is checked
against its pinned .sha256 file before anything runs.

The harness is R13's (tests/address_balances_edges_test.py) with R16's
per-reply delays: build/fortis_node_address_route_test sends every
provider host to one loopback HTTPS upstream that answers, per case, the
exact bytes TypeScript was served (a host the case marked down answers 503).
Each case's frozen instant is written to the shared clock file first; the
native provider lanes are paced by monotonic time (address_routes_test.Lanes)
and the GeckoTerminal breaker's 30 s cooldown is waited out in real time.
Each fixture group is a fresh process with its own KV directory; restart-b
reuses restart-a's after a SIGKILL.

Checks per request case: status, every recorded header (x-rate-limit-
remaining only in the quota group), the exact JSON text or protobuf bytes,
the multiset of provider requests (background revalidations included,
collected after /probe/test-address-idle), the provider request headers
TypeScript sent (accept, TRON-PRO-API-KEY) and that no client header, device
credential or provider key of another provider reaches a provider; no device
token/id or provider key reaches a response or the process output.
`seed` records persist the same news stories through the native NewsStore
(POST /probe/test-news-persist) at the same instant. `state` records compare
the device watches (an overview writes none), the media records the
popular-token logos registered (value exactly, and each expiry 1 ms before
and at the instant) and the SWR envelopes (asOf, freshUntil, staleUntil and
the stored JSON text) of the overview and holder keys.

Native-only rules (timing-dependent, not frozen by the oracle):
  - `bounded-workers`: eight distinct cold overviews at once against a
    400 ms GeckoTerminal answer 200 with their own rows; at most two
    provider requests in flight (the execution adapter's two HTTPS workers);
  - `sustained`: 120 reads from 12 clients of cached overviews (JSON,
    protobuf, HEAD) are byte-identical;
  - the production composition: the route answers 404 unless
    FORTIS_NATIVE_ADDRESS_CANARY=1, then 401/403 before any facet work.

Asserted differences are listed in ASSERTED and documented beside
handle_overview in src/wallet/address_network_routes.cpp.

Usage: network_overview_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
Environment: R18_ONLY_GROUPS=a,b limits groups; R18_FAIL_FAST=1 stops at the
first failure.
"""
import collections
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
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import address_balances_edges_test as edges  # noqa: E402
import address_insight_edges_test as insight  # noqa: E402
import address_routes_test as routes  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import tls_ca  # noqa: E402
import tron_token_holdings_edges_test as holdings  # noqa: E402

FIXTURE = "network_overview_edges.json"
check = edges.check
GECKO = "api.geckoterminal.com"
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path", "router-plural", "router-extra-segment"}
# The case that opens the GeckoTerminal lane breaker -> the case after its cooldown.
BREAKER = {"gecko-429": "gecko-breaker-recovered"}


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    print(f"{FIXTURE}: sha256 {digest} matches the pin", flush=True)
    fixture=json.loads(data)
    from strict_contract_expectations import _fields, _varint
    def rewrite(data,kind):
        result=bytearray()
        for tag,payload,raw in _fields(data):
            child={'overview':{18:'news'},'news':{18:'item'},'item':{18:'text',26:'text'}}[kind].get(tag)
            if child:
                payload=payload.replace('���'.encode(),'�'.encode()) if child=='text' else rewrite(payload,child)
                result+=_varint(tag)+_varint(len(payload))+payload
            else: result+=raw
        return bytes(result)
    for row in fixture['cases']:
        if row['name'] not in ('news-stellar','news-stellar-protobuf'): continue
        if 'wireHex' in row:
            body=rewrite(bytes.fromhex(row['wireHex']),'overview');row['wireHex']=body.hex();prefix='network-overview-protobuf'
        else:
            row['responseText']=row['responseText'].replace('���','�');body=row['responseText'].encode();prefix='network-overview'
        row['headers']['content-length']=str(len(body))
        row['headers']['etag']='W/"'+prefix+'-'+hashlib.sha256(body).hexdigest()[:24]+'"'
    return fixture


def raw(port, method, target, headers, timeout=90):
    """catalog.raw with an ordered header list (repeated names allowed)."""
    items = headers if isinstance(headers, list) else list(headers.items())
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in items]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1"))
        data = bytearray()
        while True:
            chunk = sock.recv(1 << 20)
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


def request_headers(case, kind=None):
    if case.get("headerList") is not None:
        base = [("authorization", "Bearer " + catalog.token(case["device"]))] if case["device"] else []
        return base + [tuple(pair) for pair in case["headerList"]]
    return edges.headers_of(case, kind)


# ---- asserted differences ------------------------------------------------------------------------

def parser_baseline(case, actual, observed):
    """414 and Node's raw-UTF-8 "Client Error": same status and body, the
    native security headers."""
    return edges.compare(case["name"], case, actual, baseline=True) and \
        check(not observed, f"{case['name']}: provider requests {observed}")


def restart_fresh(case, actual, observed):
    """The SWR envelope is in the durable KV natively: 10 s after the restart
    the overview entry is still fresh and is served without a GeckoTerminal
    request, where TypeScript's new process (an in-memory Kv) reads live.
    The bytes are those the first process stored, so status and every header
    but the validator are equal and only the live asOf differs."""
    status, headers, payload = actual
    try:
        native, typescript = json.loads(payload), json.loads(case["responseText"])
    except ValueError:
        return check(False, f"{case['name']}: not JSON {payload[:200]!r}")
    stored_as_of = native["popularTokens"]["asOf"]
    expected = dict(typescript, popularTokens=dict(typescript["popularTokens"], asOf=stored_as_of))
    return check(status == 200 and native == expected and stored_as_of != typescript["popularTokens"]["asOf"] and
                 not any(h == GECKO for _, h, _, _ in observed),
                 f"{case['name']}: native {payload[:400]!r} (requests {observed})")


def over_body_bound(case, actual, observed):
    """A GeckoTerminal body over 512 KiB (the provider execution bound):
    TypeScript serves its rows, native's provider execution refuses the
    body, so the popular-token card degrades exactly like an outage
    (available, no items, asOf null, stale) while the news card stays; the
    refusal is held 10 s (no second request)."""
    status, headers, payload = actual
    if case.get("wireHex"):
        return check(status == 200 and headers.get("content-type") == "application/protobuf" and not observed,
                     f"{case['name']}: native {status} {headers}")
    try:
        native, typescript = json.loads(payload), json.loads(case["responseText"])
    except ValueError:
        return check(False, f"{case['name']}: not JSON {payload[:200]!r}")
    expected = dict(typescript, popularTokens=dict(typescript["popularTokens"], items=[], asOf=None, stale=True))
    calls = [t for _, h, t, _ in observed if h == GECKO]
    want_calls = 0 if case["name"].endswith("-again") else 1
    return check(status == 200 and native == expected and len(calls) == want_calls,
                 f"{case['name']}: native {payload[:300]!r} gecko {calls}")


def head_304(case, actual, observed):
    """HEAD with a matching If-None-Match: both 304 with the same validator
    and headers; Node adds content-length: 0 to that HEAD 304, the shared
    native writer sends no Content-Length on any 304."""
    status, headers, payload = actual
    want = {k: v for k, v in case["headers"].items() if k not in ("content-length", "x-rate-limit-remaining")}
    have = {k: headers.get(k) for k in want}
    return check(case["status"] == 304 and case["headers"].get("content-length") == "0" and status == 304 and
                 have == want and "content-length" not in headers and payload == b"" and not observed,
                 f"{case['name']}: native {status} {headers}")


def repeated_header(case, actual, observed):
    """Two If-None-Match header lines: Node joins them (no validator
    matches, 200); the native request parser refuses a repeated header
    before routing (400 {"error":"BAD_REQUEST"}, no provider request)."""
    return check(case["status"] == 200 and actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def etag_of(payload, protobuf):
    digest = hashlib.sha256(payload).hexdigest()[:24]
    return f'W/"network-overview-protobuf-{digest}"' if protobuf else f'W/"network-overview-{digest}"'


def same_headers_but(case, headers, payload, protobuf):
    """Every recorded header equal except content-length / etag, which must
    describe the native body."""
    ok = True
    for name in edges.HEADERS:
        if name in ("content-length", "etag", "x-rate-limit-remaining"):
            continue
        ok = check(headers.get(name) == case["headers"].get(name),
                   f"{case['name']}: header {name} {headers.get(name)!r} != {case['headers'].get(name)!r}") and ok
    return check(headers.get("content-length") == str(len(payload)) and headers.get("etag") == etag_of(payload, protobuf),
                 f"{case['name']}: content-length/etag do not describe the native body") and ok


def l1_negative_zero(case, actual, observed):
    """A cached protobuf read 100 ms after the live one: TypeScript answers
    its in-process (L1) object, so priceUsd / change24hPct of -0 are written
    as -0.0; native answers the stored JSON, where -0 is 0 (as TypeScript's
    own KV copy has it). Equal bytes once those doubles are +0."""
    status, headers, payload = actual
    wire = bytes.fromhex(case["wireHex"])
    expected = wire
    for tag in (b"\x61", b"\x69"):
        expected = expected.replace(tag + b"\x00" * 7 + b"\x80", tag + b"\x00" * 8)
    return check(status == 200 and expected != wire and payload == expected and not observed,
                 f"{case['name']}: native bytes differ beyond -0") and same_headers_but(case, headers, payload, True)


def l1_infinite_holders(case, actual, observed, fixture):
    """Ethplorer's holdersCount 1e400 (Infinity): TypeScript's L1 object
    refuses protobuf (500) for up to 30 s; native has the stored JSON (null)
    and answers what TypeScript answers once it reads its KV copy
    (holders-infinite-cached-kv: the same entry, the same bytes)."""
    later = next(c for c in fixture["cases"] if c.get("name") == "holders-infinite-cached-kv")
    status, headers, payload = actual
    return check(case["status"] == 500 and status == 200 and payload == bytes.fromhex(later["wireHex"]) and
                 headers.get("etag") == later["headers"]["etag"] and not observed,
                 f"{case['name']}: native {status} {payload[:80]!r}")


def lane_token_retry(case, actual, observed):
    """An unparseable 2xx from GeckoTerminal: TypeScript retries inside one
    lane slot (three attempts); the native transport resubmits each parse
    retry through the lane, and GeckoTerminal's burst of two refuses the
    third. Same failure, same response (the outage card)."""
    calls = [t for _, h, t, _ in observed if h == GECKO]
    want = [c for c in case["upstream"] if urlsplit(c["url"]).netloc == GECKO]
    return edges.compare(case["name"], case, actual) and \
        check(len(want) == 3 and len(calls) == 2 and len(set(calls)) == 1, f"{case['name']}: gecko {calls}")


SHED = "holders-stale"
SHED_LATER = "holders-revalidated"


def shed_revalidation(case, actual, observed, fixture):
    """Two stale holder entries in one overview: TypeScript starts both
    background revalidations in one tick and its Ethplorer lane sheds the
    second (a call in flight), which then refreshes on the next read; the
    native revalidation worker runs them one after the other, so both
    refresh on the first read. Same responses; the request moves one read
    earlier."""
    later = next(c for c in fixture["cases"] if c.get("name") == SHED_LATER)
    early = next(c for c in fixture["cases"] if c.get("name") == SHED)
    moved = [routes.request_key(c["method"], c["url"], "") for c in later["upstream"] if "ethplorer" in c["url"]]
    base = [routes.request_key(c["method"], c["url"], "") for c in case["upstream"]]
    want = sorted(base + moved) if case["name"] == SHED else sorted(k for k in base if k not in moved)
    got = sorted(routes.request_key(m, f"https://{h}{t}", b) for m, h, t, b in observed)
    del early
    return edges.compare(case["name"], case, actual) and \
        check(len(moved) == 1 and got == want, f"{case['name']}: requests {got} != {want}")


LONG_URL = 8192  # whatwg_url kMaxInputBytes


def strip_long_logo(document):
    for item in document["popularTokens"]["items"]:
        if item["logoUrl"] and len(item["logoUrl"]) > LONG_URL:
            item["logoUrl"] = None
            item["logoMediaKey"] = None
    return document


def pb_fields(data):
    return list(holdings._fields(data))


def pb_without_long_logo(data):
    """The NetworkOverviewResponse fields with logoMediaKey/logoUrl removed
    from a token whose logoUrl exceeds the native URL bound."""
    out = []
    for number, value in pb_fields(data):
        if number == 3:
            section = []
            for n, v in pb_fields(value):
                if n == 2:
                    token = pb_fields(v)
                    if any(f == 7 and len(x) > LONG_URL for f, x in token):
                        token = [(f, x) for f, x in token if f not in (6, 7)]
                    v = ("token", token)
                section.append((n, v))
            value = ("popular", section)
        out.append((number, value))
    return out


def long_logo(case, actual, observed):
    """An image_url over 8,192 bytes: the native WHATWG parser's input bound
    refuses it, so that row has no logo (logoUrl and logoMediaKey null, no
    media record) where TypeScript keeps both; every other field equal."""
    status, headers, payload = actual
    if "wireHex" in case:
        ok = status == 200 and pb_without_long_logo(payload) == pb_without_long_logo(bytes.fromhex(case["wireHex"]))
        return check(ok and len(payload) < len(case["wireHex"]) // 2, f"{case['name']}: protobuf differs") and \
            same_headers_but(case, headers, payload, True)
    try:
        native = json.loads(payload)
    except ValueError:
        return check(False, f"{case['name']}: not JSON")
    expected = strip_long_logo(json.loads(case["responseText"]))
    return check(status == 200 and native == expected and not native == json.loads(case["responseText"]),
                 f"{case['name']}: native {payload[:300]!r}") and same_headers_but(case, headers, payload, False) and \
        edges.compare_upstream(case["name"], case, observed, [])


# news-stellar / news-stellar-protobuf (a seeded story with lone surrogates
# in its headline and summary) are compared exactly: since R28–R30 the native
# news store writes story text as TypeScript's lmdb/msgpack round trip
# returns it (three U+FFFD per lone surrogate in a string shorter than 64
# units, one otherwise; news_store.cpp append_stored).


ASSERTED = {
    "inm-head": head_304,
    "prices-protobuf": l1_negative_zero,
    "prices-b-protobuf": l1_negative_zero,
    "gecko-not-json": lane_token_retry,
    "gecko-truncated": lane_token_retry,
    "gecko-html": lane_token_retry,
    "long-logo-url": long_logo,
    "long-logo-url-protobuf": long_logo,
    "inm-twice": repeated_header,
    "network-malformed-escape": edges.bad_url,
    "fragment": insight.fragment_refusal,
    "raw-latin1-network": insight.invalid_utf8_path,
    "raw-utf8-network": parser_baseline,
    "raw-utf8-query": parser_baseline,
    "restart-after-fresh": restart_fresh,
    "huge-over-512kib": over_body_bound,
    "huge-over-512kib-protobuf": over_body_bound,
    "huge-over-512kib-again": over_body_bound,
}


# ---- provider request identity -------------------------------------------------------------------

def provider_headers(h, case, before, count):
    """Every native provider request carries the accept / TRON-PRO-API-KEY
    headers TypeScript's request with the same URL carried, no client
    header, and no other provider's key."""
    expected = {}
    for call in case.get("upstream", []):
        expected.setdefault(routes.request_key(call["method"], call["url"], call.get("requestBody") or ""),
                            call.get("headers") or {})
    keys = h.fixture["providerKeys"]
    owner = {keys[0]: "api.etherscan.io", keys[1]: "apilist.tronscanapi.com", keys[2]: "api.ethplorer.io"}
    ok = True
    calls = h.upstream.calls()[before:before + count]
    sent_headers = h.upstream.call_headers()[before:before + count]
    for (method, host, target, body), sent in zip(calls, sent_headers):
        text = f"{host}{target} {body} {json.dumps(sent)}"
        for key, host_of_key in owner.items():
            if host != host_of_key:
                ok = check(key not in text, f"{case['name']}: {host} received the {host_of_key} key") and ok
        for leaked in ("authorization", "cookie", "x-forwarded-for", "origin"):
            ok = check(leaked not in sent, f"{case['name']}: {host} request carries {leaked}") and ok
        want = expected.get(routes.request_key(method, f"https://{host}{target}", body))
        if want is None:
            continue  # compare_upstream reports the unexpected request
        for name in ("accept", "tron-pro-api-key"):
            if name in want or name in sent:
                ok = check(sent.get(name) == want.get(name),
                           f"{case['name']}: {host} {name} {sent.get(name)!r} != {want.get(name)!r}") and ok
    return ok


# ---- state and seed records ----------------------------------------------------------------------

def post(server, path, document):
    status, _, payload = server.request("POST", path, body=json.dumps(document).encode(),
                                        headers={"Content-Type": "application/json"})
    return status, payload


def replay_seed(h, server, case):
    harness.set_clock(h.clock, case["at"])
    stories = case["seed"]
    ok = True
    for offset in range(0, len(stories), 20):
        status, payload = post(server, "/probe/test-news-persist", {"stories": stories[offset:offset + 20]})
        ok = check(status == 200, f"{case['name']}: news persist {status} {payload[:200]!r}") and ok
    return ok


def media_probe(server, keys, now=None):
    document = {"accounts": [], "mediaKeys": keys}
    if now is not None:
        document["now"] = now
    status, payload = post(server, "/probe/test-nft-state", document)
    return json.loads(payload)["media"] if status == 200 else None


def state_expectation(h, case):
    """The state a recorded difference changes natively (asserted):
    - bounds-state: no media record for the over-long logo URL, and no
      overview entry (the over-512 KiB read failed natively);
    - restart-state: the entry the first process wrote (durable KV), not
      TypeScript's live re-read after the restart;
    - swr-holders: the second holder entry refreshed at holders-stale (see
      shed_revalidation) instead of holders-revalidated."""
    media = [dict(m) for m in case["media"]]
    envelopes = [json.loads(json.dumps(e)) for e in case["envelopes"]]
    asserted = False
    by_name = {c.get("name"): c for c in h.fixture["cases"]}
    if case["name"] == "bounds-state":
        for m in media:
            if "value" in m and len(m["value"]["sourceUrl"]) > LONG_URL:
                media[media.index(m)] = {"key": m["key"], "missing": True}
                asserted = True
        for e in envelopes:
            if e["key"] == "netoverview:v3:base":
                e["envelope"] = None
    if case["name"] == "restart-state":
        before = by_name["restart-before"]["at"]
        for e in envelopes:
            if e["key"] == "netoverview:v3:base" and e["envelope"]:
                shift = e["envelope"]["asOf"] - before
                e["envelope"] = dict(e["envelope"], asOf=before, freshUntil=e["envelope"]["freshUntil"] - shift,
                                     staleUntil=e["envelope"]["staleUntil"] - shift)
                asserted = shift > 0
    if case["name"] == "swr-holders":
        shed_at, later_at = by_name[SHED]["at"], by_name[SHED_LATER]["at"]
        moved = [c["url"].split("/getTokenInfo/")[1].split("?")[0]
                 for c in by_name[SHED_LATER]["upstream"] if "ethplorer" in c["url"]]
        for e in envelopes:
            if e["envelope"] and any(address in e["key"] for address in moved):
                shift = later_at - shed_at
                e["envelope"] = dict(e["envelope"], asOf=e["envelope"]["asOf"] - shift,
                                     freshUntil=e["envelope"]["freshUntil"] - shift,
                                     staleUntil=e["envelope"]["staleUntil"] - shift)
                asserted = True
    return media, envelopes, asserted


def replay_state(h, server, case):
    name = case["name"]
    media_want, envelopes_want, _ = state_expectation(h, case)
    harness.set_clock(h.clock, case["at"])
    ok = True
    for letter in case["state"]["devices"]:
        status, payload = post(server, "/probe/test-address-state",
                               {"device": h.fixture["deviceIds"][letter], "accounts": []})
        got = json.loads(payload) if status == 200 else {}
        ok = check(got.get("watches") == case["watches"][letter],
                   f"{name}: watches of {letter} {got.get('watches')} != {case['watches'][letter]}") and ok
    keys = [m["key"] for m in case["media"]]
    got = media_probe(server, keys)
    if not check(got is not None and len(got) == len(keys), f"{name}: media probe"):
        return False
    for want, have in zip(media_want, got):
        expected = {k: v for k, v in want.items() if k != "expiresAt"}
        ok = check(have == expected, f"{name}: media {want['key']}\n  expected {expected}\n  actual   {have}") and ok
    for instant in sorted({m["expiresAt"] for m in media_want if m.get("expiresAt")}):
        for at in (instant - 1, instant):
            seen = media_probe(server, keys, at)
            want = [("value" in m) and m["expiresAt"] > at for m in media_want]
            ok = check(seen is not None and ["value" in m for m in seen] == want,
                       f"{name}: media visible at {at} (expiry {instant})") and ok
    status, payload = post(server, "/probe/test-address-swr", {"keys": [e["key"] for e in case["envelopes"]]})
    if not check(status == 200, f"{name}: swr probe {status}"):
        return False
    for want, have in zip(envelopes_want, json.loads(payload)["envelopes"]):
        ok = check(have == want, f"{name}: {want['key'][:80]}\n  expected {json.dumps(want)[:900]}\n"
                                 f"  actual   {json.dumps(have)[:900]}") and ok
    return ok


# ---- request cases --------------------------------------------------------------------------------

def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        if not replay_state(h, server, case):
            return "failed"
        return "asserted" if state_expectation(h, case)[2] else "matched"
    if "seed" in case:
        return "matched" if replay_seed(h, server, case) else "failed"
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
            if entry.get("startDelayMs"):
                time.sleep(entry["startDelayMs"] / 1000)
            try:
                results[index] = raw(server.port, "HEAD" if entry["kind"] == "head" else "GET",
                                     entry.get("target") or target, request_headers(case, entry["kind"]))
            except (OSError, IndexError, ValueError) as error:
                results[index] = (0, {}, repr(error).encode())

        threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
        edges.repay(lanes, case, observed)
        ok = True
        for index, (record, actual) in enumerate(zip(case["responses"], results)):
            h.no_secret(actual[2], name)
            ok = edges.compare(f"{name}[{index}:{kinds[index]['kind']}]", record, actual) and ok
        ok = edges.compare_upstream(name, case, observed, h.request_secrets) and ok
        ok = provider_headers(h, case, before, len(observed)) and ok
        return "matched" if ok else "failed"
    try:
        actual = raw(server.port, case["method"], target, request_headers(case))
    except OSError as error:
        check(False, f"{name}: {error!r} (TypeScript {case['status']})")
        edges.settle(server, h.upstream, before, len(case["upstream"]))
        return "failed"
    h.no_secret(actual[2], name)
    observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
    edges.repay(lanes, case, observed)
    placed = provider_headers(h, case, before, len(observed))
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(actual[0], actual[1], actual[2], observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        ok = check(case["status"] == 404 and actual[0] == 404 and not observed, f"{name}: status {actual[0]} != 404")
        return "asserted" if ok and placed else "failed"
    if name in ("holders-infinite-protobuf", "holders-infinite-cached-l1"):
        return "asserted" if l1_infinite_holders(case, actual, observed, h.fixture) and placed else "failed"
    if name in (SHED, SHED_LATER):
        return "asserted" if shed_revalidation(case, actual, observed, h.fixture) and placed else "failed"
    rule = ASSERTED.get(name) or (parser_baseline if case["status"] in (414, 431) else None)
    if rule:
        return "asserted" if rule(case, actual, observed) and placed else "failed"
    ok = edges.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = edges.compare_upstream(name, case, observed, h.request_secrets) and ok
    return "matched" if ok and placed else "failed"


# ---- native-only ------------------------------------------------------------------------------------

def bounded_workers(h, fixture):
    """Eight distinct cold overviews at once, GeckoTerminal answering after
    400 ms: each answers 200 with its own row; never more than two provider
    requests overlap (the execution adapter's HTTPS workers; the
    GeckoTerminal lane itself allows two)."""
    networks = ["base", "polygon", "arbitrum", "optimism", "avalanche", "cronos", "hyperevm", "flare"]

    def answer(method, host, target, body):
        slug = target.split("/networks/", 1)[1].split("/", 1)[0] if host == GECKO else ""
        address = "0x" + hashlib.sha256(slug.encode()).hexdigest()[:40]
        document = {"data": [{"id": "p", "type": "pool", "attributes": {"market_cap_usd": "5"},
                              "relationships": {"base_token": {"data": {"id": "t", "type": "token"}}}}],
                    "included": [{"id": "t", "type": "token", "attributes": {"address": address, "name": slug,
                                                                             "symbol": "b", "decimals": 6}}]}
        return 200, json.dumps(document).encode(), 400, True

    group = next(g for g in fixture["groups"] if g["name"] == "shapes")
    server = h.server("bounded", group["env"], fixture["now"] + 40 * 86400000)
    ok = True
    try:
        h.table.override = answer
        h.table.spans = []
        results = [None] * len(networks)
        barrier = threading.Barrier(len(networks))

        def one(index):
            barrier.wait()
            results[index] = raw(server.port, "GET", f"/v1/networks/{networks[index]}/overview",
                                 {"authorization": "Bearer " + catalog.token("A")})

        threads = [threading.Thread(target=one, args=(i,)) for i in range(len(networks))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        served = 0
        for index, result in enumerate(results):
            document = json.loads(result[2]) if result and result[0] == 200 else {}
            items = document.get("popularTokens", {}).get("items", [])
            # The lane allows two at once and ten a minute: reads it refuses
            # degrade to the outage card (stale, no items), never an error.
            fine = result[0] == 200 and (
                (len(items) == 1 and items[0]["name"] == {"base": "base", "polygon": "polygon_pos",
                                                         "arbitrum": "arbitrum", "optimism": "optimism",
                                                         "avalanche": "avax", "cronos": "cro",
                                                         "hyperevm": "hyperevm", "flare": "flare"}[networks[index]])
                or (items == [] and document["popularTokens"]["stale"] is True))
            served += len(items)
            ok = check(fine, f"bounded-workers[{networks[index]}]: {result[0]} {result[2][:160]!r}") and ok
        events = sorted([(start, 1) for start, _ in h.table.spans] + [(end, -1) for _, end in h.table.spans])
        peak = current = 0
        for _, delta in events:
            current += delta
            peak = max(peak, current)
        ok = check(peak <= 2 and served == len(h.table.spans) and served >= 2,
                   f"bounded-workers: {len(h.table.spans)} provider requests, {served} served, peak {peak}") and ok
        print(f"bounded-workers: 8 cold overviews, {len(h.table.spans)} GeckoTerminal requests "
              f"({served} served, the rest refused by the lane), peak in flight {peak}", flush=True)
    finally:
        h.table.override = None
        server.stop()
        h.no_secret(server.process.stdout.read() + server.process.stderr.read(), "bounded output")
    return ok


def sustained(h, fixture):
    """120 reads of cached overviews from 12 clients (JSON, protobuf, HEAD):
    every answer is byte-identical to the first of its kind."""
    group = next(g for g in fixture["groups"] if g["name"] == "keys")
    server = h.server("sustained", {}, fixture["now"] + 41 * 86400000)
    ok = True
    try:
        h.table.override = lambda method, host, target, body: (200, b'{"data":[]}', 5, True)
        first = {}
        for kind in ("json", "protobuf", "head"):
            headers = {"authorization": "Bearer " + catalog.token("A")}
            if kind == "protobuf":
                headers["accept"] = "application/protobuf"
            first[kind] = raw(server.port, "HEAD" if kind == "head" else "GET", "/v1/networks/base/overview", headers)
        lock = threading.Lock()
        outcomes = []

        def client(offset):
            for index in range(offset, 120, 12):
                kind = ("json", "protobuf", "head")[index % 3]
                headers = {"authorization": "Bearer " + catalog.token("A" if index % 2 else "B")}
                if kind == "protobuf":
                    headers["accept"] = "application/protobuf"
                try:
                    result = raw(server.port, "HEAD" if kind == "head" else "GET", "/v1/networks/base/overview",
                                 headers)
                except OSError as error:
                    result = (0, {}, repr(error).encode())
                with lock:
                    outcomes.append((kind, result))

        threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        same = sum(1 for kind, result in outcomes if result[0] == 200 and result[2] == first[kind][2] and
                   result[1].get("etag") == first[kind][1].get("etag"))
        ok = check(same == 120, f"sustained: {same}/120 identical") and ok
        print(f"sustained: {same}/120 identical cached reads from 12 clients", flush=True)
        del group
    finally:
        h.table.override = None
        server.stop()
        h.no_secret(server.process.stdout.read() + server.process.stderr.read(), "sustained output")
    return ok


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/bitcoin/overview"
    for value in (None, "0"):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ADDRESS_CANARY"] = value
        server = catalog.Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            status, _, _ = raw(server.port, "GET", path, {"authorization": "Bearer " + catalog.token("a")})
            check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            server.stop()
    server = catalog.Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ADDRESS_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca),
                                                           "ETHERSCAN_API_KEY": fixture["providerKeys"][0]})
    try:
        status, headers, body = raw(server.port, "GET", path, {})
        check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
              f"production no-auth: {status} {body!r}")
        status, _, body = raw(server.port, "GET", path, {"origin": "https://evil.example"})
        check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, body = raw(server.port, "GET", "/v1/networks/bitcoin%00/overview", {})
        # opaque-params: the router refuses the NUL before any hook.
        check(status == 404 and body == b'{"error":"NOT_FOUND"}',
              f"production NUL network without a bearer: {status} {body!r}")
    finally:
        server.stop()
        output = server.process.stdout.read() + server.process.stderr.read()
        check(fixture["providerKeys"][0].encode() not in output, "production output carries the Etherscan key")
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R18_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="network-overview-") as root:
        root = Path(root)
        h = edges.Harness(binary, root, fixture)
        h.table = holdings.DelayedTable(fixture["cases"])
        h.upstream.table = h.table
        # Provider keys are checked per owning host (provider_headers); the
        # device secrets may reach no provider at all.
        h.request_secrets = list(h.secrets)
        h.secrets += list(fixture["providerKeys"])
        opened = {}
        try:
            for name, cases in by_group.items():
                if only and name not in only and not (name.startswith("restart") and "restart" in only):
                    continue
                group = groups[name]
                server = h.server(group["kv"], group["env"], group["start"])
                lanes = routes.Lanes()
                results = collections.Counter()
                try:
                    if name == "quota":
                        catalog.wait_for_fresh_minute(25)
                        for _ in range(240):
                            status, _, _ = raw(server.port, "GET", "/v1/networks/nowhere/overview",
                                               {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in cases:
                        for opener, after in BREAKER.items():
                            if after == case["name"] and opener in opened:
                                wait = opened[opener] + 31 - time.monotonic()
                                if wait > 0:
                                    time.sleep(wait)
                        outcome = replay_case(h, server, case, lanes)
                        results[outcome] += 1
                        if os.environ.get("R18_TRACE"):
                            print(f"  {name}/{case['name']}: {outcome}", flush=True)
                        if case["name"] in BREAKER:
                            opened[case["name"]] = time.monotonic()
                        if os.environ.get("R18_FAIL_FAST") and edges.FAILURES:
                            raise SystemExit(1)
                finally:
                    if name == "restart-a":
                        server.process.send_signal(signal.SIGKILL)
                        server.process.wait()
                    else:
                        server.stop()
                    h.no_secret(server.process.stdout.read() + server.process.stderr.read(), f"{name} output")
                totals.update(results)
                print(f"{name}: {results['matched']} matched, {results['asserted']} asserted differences, "
                      f"{results['failed']} failed", flush=True)
            if not only or "bounded" in only:
                totals["matched" if bounded_workers(h, fixture) else "failed"] += 1
                totals["matched" if sustained(h, fixture) else "failed"] += 1
        finally:
            h.upstream.close()
        if h.table.extra:
            print(f"requests TypeScript never made: {sorted(set(h.table.extra))[:10]}")
        if len(sys.argv) == 3 and not only:
            production(sys.argv[2], root, fixture)
    print(f"network overview: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    print("network overview edges: all checks passed")


if __name__ == "__main__":
    main()
