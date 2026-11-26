#!/usr/bin/env python3
"""Real-socket edge tests of GET /v1/media/{key} (R08) on
build/fortis_node_news_route_test, against the TypeScript oracle
tests/corpus/media_get_edges.json
(recorded by the earlier TypeScript implementation's tests).

Replay: every oracle step runs at the fixture's clock on one KV directory.
Registrations go through /probe/test-media-register (RegisterMediaSource, the
code every native token/market/news/NFT read uses) and must mint the oracle's
keys. Requests go out on raw sockets (one connection per request, parallel
where the oracle was parallel); each must match the oracle's status, header
set (plus the native security baseline where TypeScript sends none), body
(JSON, or sha256 + length of the bytes) and the upstream calls it caused (the
URLs the production TypeScript transport would put on the wire). After each
step the stored record (/probe/test-media-record) must match the oracle's
StoredMedia fields. The loopback upstream serves the oracle's reply recipes
byte for byte with the declared framing (Content-Length, a declared length,
or chunked), delays, hangs and resets; the oracle's restart is a SIGKILL.

Asserted native differences (EXPECTED): an IPv6 literal and an IPv6-only
host are refused before any request (the guest stack is IPv4-only, AAAA is
never consulted), and a host whose AAAA answer is private but A answer public
is fetched over IPv4; the router's own 404 body for a trailing slash or
another method.

Native-only blocks after the replay: privacy (no client header reaches the
upstream, the server log names no source URL), the orphan-chunk prune, the
byte-retention window (chunks expire seven days after their last write),
distinct-key pressure past the 16 job/handoff slots, and cancellation (SIGTERM
during a hung fetch exits promptly, answers the waiting readers 404 and
records nothing, so the key is fetched after the restart).

R08_FAST=1 skips the 10 s transport-timeout step and its cooldown check
(mutation runs).
Usage: media_get_route_test.py <build/fortis_node_news_route_test>
"""
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import sys
import tempfile
import threading
import time
import urllib.parse

import news_route_harness as harness
import compat_expectations as ce
import tls_ca

FAST = os.environ.get("R08_FAST") == "1"
HEADERS = ["content-type", "content-length", "cache-control", "x-content-type-options",
           "content-security-policy", "content-disposition", "referrer-policy", "permissions-policy",
           "vary", "x-rate-limit-cost", "access-control-allow-origin"]
# Every native response carries the server baseline; the root-scoped
# TypeScript route sends none of it (an accepted difference).
BASELINE = {"x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
            "permissions-policy": "camera=(), microphone=(), geolocation=()",
            "cache-control": "no-store"}
STATE_FIELDS = ["status", "contentType", "sizeBytes", "fetchedAt", "lastAccessAt", "fetchCount",
                "updatedAt"]
ROUTER_404 = {"error": "NOT_FOUND"}
DAY = 86_400_000
H = 3_600_000


def native_ok(state):
    return dict(state, status="ok", hasBytes=True)


EXPECTED = {
    # IPv6 is refused natively before any packet (guest lwIP is IPv4-only).
    "ipv6Public": {"status": 404, "body": ROUTER_404, "calls": [], "replies": {},
                   "state": "failed_permanent"},
    "dnsIpv6Only": {"status": 404, "body": ROUTER_404, "calls": [], "replies": {},
                    "state": "failed_permanent"},
    # Only the A answer is consulted: fetched where TypeScript refuses.
    "dnsIpv6Private": {"status": 200, "calls": ["https://v6private.example/x.png"],
                       "replies": {"https://v6private.example/x.png": [
                           {"status": 200, "headers": [["Content-Type", "image/png"]],
                            "body": {"kind": "png", "size": 911, "seed": 36}}]},
                       "state": "ok"},
    # The router's own 404 (status matches; Fastify's body names the route).
    "trailingSlash": {"status": 404, "body": ROUTER_404},
    "postMethod": {"status": 404, "body": ROUTER_404},
}


def body_of(spec, signatures):
    if spec is None:
        return b""
    if "text" in spec:
        return spec["text"].encode("latin1")
    size, seed = spec["size"], spec["seed"]
    body = bytearray((i * 131 + seed) & 255 for i in range(size))
    signature = bytes.fromhex(signatures[spec["kind"]])[:size]
    body[:len(signature)] = signature
    return bytes(body)


class Upstream:
    """Raw loopback upstream: TLS and plaintext listeners. The URL of a
    request is rebuilt from x-fortis-test-host; replies come from the current
    step's queue per URL and are written exactly as declared."""

    def __init__(self, root, signatures):
        self.signatures = signatures
        self.lock = threading.Lock()
        self.replies = {}
        self.observed = []  # (url, headers)
        self.stop = threading.Event()
        self.hung = []
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.listeners = []
        for scheme in ("https", "http"):
            sock = socket.socket()
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("127.0.0.1", 0))
            sock.listen(64)
            self.listeners.append((scheme, sock))
            threading.Thread(target=self.accept, args=(scheme, sock), daemon=True).start()
        self.port = self.listeners[0][1].getsockname()[1]
        self.plain_port = self.listeners[1][1].getsockname()[1]

    def set_replies(self, replies):
        with self.lock:
            self.replies = {url: list(queue) for url, queue in replies.items()}

    def unused(self):
        with self.lock:
            return {url: len(queue) for url, queue in self.replies.items() if queue}

    def calls(self):
        with self.lock:
            return list(self.observed)

    def accept(self, scheme, sock):
        while not self.stop.is_set():
            try:
                client, _ = sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(scheme, client), daemon=True).start()

    def serve(self, scheme, client):
        try:
            if scheme == "https":
                client = self.context.wrap_socket(client, server_side=True)
            client.settimeout(30)
            head = b""
            while b"\r\n\r\n" not in head:
                chunk = client.recv(65536)
                if not chunk:
                    return
                head += chunk
            lines = head.split(b"\r\n\r\n", 1)[0].decode("latin1").split("\r\n")
            path = lines[0].split(" ")[1]
            headers = {}
            for line in lines[1:]:
                name, _, value = line.partition(":")
                headers[name.strip().lower()] = value.strip()
            host, _, port = headers.get("x-fortis-test-host", "?:0").rpartition(":")
            default = (scheme, port) in (("https", "443"), ("http", "80"))
            url = f"{scheme}://{host}{path}" if default else f"{scheme}://{host}:{port}{path}"
            with self.lock:
                self.observed.append((url, headers))
                queue = self.replies.get(url)
                reply = queue.pop(0) if queue else None
            if reply is None:
                client.sendall(b"HTTP/1.1 599 Unexpected\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            self.answer(client, reply)
        except OSError:
            pass
        finally:
            try:
                client.close()
            except OSError:
                pass

    def answer(self, client, reply):
        if reply.get("reset"):
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            return
        if reply.get("hang"):
            self.hung.append(client)
            deadline = time.time() + 20
            while time.time() < deadline and not self.stop.is_set():
                try:
                    client.settimeout(.2)
                    if client.recv(1) == b"":
                        return  # the server gave up
                except socket.timeout:
                    continue
                except OSError:
                    return
            return
        if reply.get("delayMs"):
            time.sleep(reply["delayMs"] / 1000)
        status = reply["status"]
        body = body_of(reply.get("body"), self.signatures)
        framing = reply.get("framing", "length")
        head = [f"HTTP/1.1 {status} Reply"]
        for name, value in reply.get("headers", []):
            head.append(f"{name}: {value}")
        null_body = status in (101, 204, 205, 304)
        if framing == "length" and not null_body:
            head.append(f"Content-Length: {len(body)}")
        elif framing == "declared":
            head.append(f"Content-Length: {reply['declared']}")
        elif framing == "stream":
            head.append("Transfer-Encoding: chunked")
        head.append("Connection: close")
        client.settimeout(30)
        client.sendall(("\r\n".join(head) + "\r\n\r\n").encode("latin1"))
        if reply.get("stallAfterHeaders"):
            time.sleep(3)
            return
        if null_body:
            return
        if framing == "stream":
            for offset in range(0, len(body), 65536):
                piece = body[offset:offset + 65536]
                client.sendall(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
            client.sendall(b"0\r\n\r\n")
        elif framing == "length":
            client.sendall(body)

    def close(self):
        self.stop.set()
        for _, sock in self.listeners:
            sock.close()


def raw(port, method, target, headers=None, timeout=60):
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1"))
        data = bytearray()
        while True:
            chunk = sock.recv(1 << 20)
            if not chunk:
                break
            data.extend(chunk)
    if not data:
        return None
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    first, *rest = head.decode("latin1").split("\r\n")
    response_headers = {}
    for line in rest:
        name, _, value = line.partition(":")
        response_headers[name.strip().lower()] = value.strip()
    return int(first.split()[1]), response_headers, payload


def describe(response):
    status, headers, payload = response
    out = {"status": status, "headers": {h: headers[h] for h in HEADERS if h in headers}}
    if not payload:
        out["bodyLength"] = 0
    elif headers.get("content-type", "").startswith("image/"):
        out["bodySha256"] = hashlib.sha256(payload).hexdigest()
        out["bodyLength"] = len(payload)
    else:
        try:
            out["body"] = json.loads(payload)
        except ValueError:
            out["bodyText"] = payload.decode("latin1")
    return out


def key_of(url):
    return hashlib.sha256(url.encode("utf-8")).hexdigest()[:32]


class Native:
    def __init__(self, binary, root, upstream, dns):
        self.binary, self.root, self.upstream, self.dns = binary, Path(root), upstream, dns
        self.clock = self.root / "clock"
        self.kv = self.root / "kv"
        self.kv.mkdir(parents=True, exist_ok=True)
        self.server = None

    def start(self, at):
        harness.set_clock(self.clock, at)
        self.server = harness.Server(self.binary, self.kv, self.clock, self.upstream,
                                     env={"FORTIS_TEST_MEDIA_DNS": self.dns})

    def set_clock(self, at):
        harness.set_clock(self.clock, at)

    def register(self, url):
        reply = self.server.probe("/probe/test-media-register?url=" + urllib.parse.quote(url, safe=""))
        return reply["key"]

    def record(self, key):
        return self.server.probe(f"/probe/test-media-record?key={key}")

    def prune(self):
        return self.server.probe("/probe/test-media-prune")

    def get(self, method, target, headers=None):
        return raw(self.server.port, method, target, headers)


def compare_state(native, url, expected, fail):
    record = native.record(key_of(url))
    actual = record["record"]
    if not expected["present"]:
        if actual is not None:
            fail(f"state {url}: present natively ({actual['status']}), absent in TypeScript")
        return record
    if actual is None:
        fail(f"state {url}: absent natively")
        return record
    if (actual["bytes"] is not None) != expected["hasBytes"]:
        fail(f"state {url}: hasBytes {actual['bytes'] is not None} != {expected['hasBytes']}")
    for field in STATE_FIELDS:
        if actual.get(field) != expected.get(field):
            fail(f"state {url}: {field} {actual.get(field)!r} != {expected.get(field)!r}")
    if actual["bytes"] is not None:
        sizes = [size for size in record["chunks"] if size is not None]
        if sum(sizes) != actual["sizeBytes"]:
            fail(f"state {url}: chunks {record['chunks']} do not hold {actual['sizeBytes']} bytes")
    return record


def compare_response(name, expected, actual, fail, expected_override=None, *, target="", method="GET"):
    if actual is None:
        fail(f"{name}: connection closed without a response")
        return
    got = describe(actual)
    want = expected
    # Pin the native refusal (opaque-params), including HEAD's size.
    if not expected_override and ce.malformed_path_target({"target": target}):
        headers = dict(expected["headers"], **{"content-type": "application/json",
                                                "content-length": str(len(ce.ROUTER_NOT_FOUND))})
        headers.pop("x-rate-limit-cost", None)
        want = describe((404, headers, b"" if method == "HEAD" else ce.ROUTER_NOT_FOUND))
    elif not expected_override and expected["status"] == 414:
        recorded = {"code": "FST_ERR_MAX_PARAM_LENGTH", "statusCode": 414}
        status, headers, body, _ = ce.fastify_expected(414, expected["headers"], recorded)
        want = describe((status, headers, b"" if method == "HEAD" else body))
    if expected_override:
        if got["status"] != expected_override["status"]:
            fail(f"{name}: status {got['status']} != {expected_override['status']} (asserted difference)")
        if "body" in expected_override and got.get("body") != expected_override["body"]:
            fail(f"{name}: body {got.get('body')!r} != {expected_override['body']!r}")
        return
    if got["status"] != want["status"]:
        fail(f"{name}: status {got['status']} != {want['status']} {got.get('body')!r}")
    for header in HEADERS:
        expected_value = want["headers"].get(header)
        actual_value = got["headers"].get(header)
        if expected_value is None and actual_value == BASELINE.get(header):
            continue
        if actual_value != expected_value:
            fail(f"{name}: header {header} {actual_value!r} != {expected_value!r}")
    for field in ("body", "bodyText", "bodySha256", "bodyLength"):
        if want.get(field) != got.get(field):
            fail(f"{name}: {field} {str(got.get(field))[:200]!r} != {str(want.get(field))[:200]!r}")


def replay(fixture, binary, root, upstream, dns, failures):
    native = Native(binary, root, upstream, dns)
    steps = fixture["steps"]
    first_at = next(step["at"] for step in steps if "at" in step)
    native.start(first_at)
    counted = 0
    try:
        for step in steps:
            name = step.get("name", step["op"])

            def fail(message, name=name):
                failures.append(f"{name}: {message}")

            if step["op"] == "restart":
                native.server.kill()  # hard: SIGKILL, nothing flushed on exit
                native.start(first_at)
                continue
            native.set_clock(step["at"])
            if step["op"] == "register":
                keys = [native.register(url) for url in step["urls"]]
                if keys != step["keys"]:
                    fail(f"keys {keys} != {step['keys']}")
                continue
            if step["op"] == "prune":
                result = native.prune()
                if result["freed"] != step["freed"]:
                    fail(f"freed {result['freed']} != {step['freed']}")
                print(f"  {name}: freed {result['freed']} (TypeScript {step['freed']}), "
                      f"{result['orphans']} orphan chunks deleted (native only)")
                for state in step["state"]:
                    compare_state(native, state["url"], state, fail)
                continue
            if FAST and name in ("transportTimeout", "timeoutCooldown"):
                upstream.set_replies({})
                continue
            override = EXPECTED.get(name)
            replies = override["replies"] if override and "replies" in override else step["replies"]
            upstream.set_replies(replies)
            before = len(upstream.calls())
            targets = step["targets"]
            parallel = len(step["responses"])
            if len(targets) == 1:
                targets = targets * parallel
            methods = ["HEAD" if name == "joinedHeadMix" and i % 2 else step["methods"][0]
                       for i in range(parallel)]
            answers = [None] * parallel
            started = time.time()

            def run(index):
                answers[index] = native.get(methods[index], targets[index], step["requestHeaders"])
            threads = [threading.Thread(target=run, args=(i,)) for i in range(parallel)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            elapsed = time.time() - started
            for index, expected in enumerate(step["responses"]):
                compare_response(f"{name}[{index}]" if parallel > 1 else name, expected,
                                 answers[index], fail, override, target=targets[index], method=methods[index])
            time.sleep(.05)
            observed = [url for url, _ in upstream.calls()[before:]]
            expected_calls = override["calls"] if override and "calls" in override else \
                [call["url"] for call in step["calls"]]
            if sorted(observed) != sorted(expected_calls) or (
                    parallel == 1 and observed != expected_calls):
                fail(f"upstream {observed} != {expected_calls}")
            for _, headers in upstream.calls()[before:]:
                if headers.get("accept") != "image/*":
                    fail(f"accept {headers.get('accept')!r}")
            if upstream.unused():
                fail(f"replies never requested {upstream.unused()}")
            if name == "transportTimeout" and not (9.5 <= elapsed < 15):
                fail(f"transport timeout answered after {elapsed:.1f} s (10 s hop timeout)")
            for state in step["state"]:
                if override and "state" in override:
                    record = native.record(key_of(state["url"]))["record"]
                    if record is None or record["status"] != override["state"]:
                        fail(f"state {record and record['status']} != {override['state']} (asserted)")
                    continue
                compare_state(native, state["url"], state, fail)
            counted += 1
    finally:
        log = native.server.log()
        native.server.stop()
    return native, counted, log


def privacy(upstream, log, failures):
    # The transport's own request headers; nothing the client sent.
    allowed = {"host", "accept", "accept-encoding", "connection", "user-agent", "x-fortis-test-host"}
    for url, headers in upstream.calls():
        extra = set(headers) - allowed
        if extra:
            failures.append(f"privacy: {url} received client-side headers {sorted(extra)}")
        if headers.get("user-agent") != "FortisNetWallet/1.0" or \
                headers.get("accept-encoding") != "identity" or headers.get("connection") != "close":
            failures.append(f"privacy: transport headers {headers}")
    for secret in ("fixture-secret-7731", "user:secret", "203.0.113.9", "session=abc", "WalletApp"):
        if secret in log:
            failures.append(f"privacy: the server log contains {secret!r}")
    print(f"  privacy: {len(upstream.calls())} upstream requests carried only the transport's "
          "host/accept/accept-encoding/connection/user-agent; the server log names no source URL "
          "or client header")


def orphan_prune(native, fixture, failures):
    """After the replay every record has expired. The replay's own prunes
    already deleted the chunks of records that had expired by then; this
    prune deletes the rest (the stored png's), then finds none."""
    native.start(fixture["steps"][-1]["at"] + DAY)
    try:
        png = key_of("https://cdn.example/a.png")
        before = native.record(png)
        first = native.prune()
        second = native.prune()
        after = native.record(png)
        if first["orphans"] < 1 or second["orphans"] != 0 or first["freed"] != 0:
            failures.append(f"orphan prune: {first} then {second}")
        if before["record"] is not None or after["chunks"] != [None] * 4:
            failures.append(f"orphan prune: png {before} -> {after}")
        print(f"  orphan prune: {first['orphans']} remaining orphan chunk(s) deleted, then none")
    finally:
        native.server.stop()


def retention_window(binary, root, upstream, failures):
    """Native byte retention: chunks expire seven days after their last
    write; a view rewrites them only a day or more after it. A view one hour
    after the fetch extends the record to day 7 + 1 h but not the chunks, so
    a read at day 7 + 30 min re-fetches natively (TypeScript would still
    serve the stored bytes) and a read at day 7 + 2 h finds no record."""
    native = Native(binary, root, upstream, "cdn.example=93.184.216.34")
    t = 1_806_000_000_000
    url = "https://cdn.example/window.png"
    reply = {"status": 200, "headers": [["Content-Type", "image/png"]],
             "body": {"kind": "png", "size": 3000, "seed": 77}}
    native.start(t)
    try:
        native.register(url)
        upstream.set_replies({url: [reply, reply]})
        path = f"/v1/media/{key_of(url)}"
        before = len(upstream.calls())
        statuses = [native.get("GET", path)[0]]
        native.set_clock(t + H)
        statuses.append(native.get("GET", path)[0])
        native.set_clock(t + 7 * DAY + 30 * 60_000)
        statuses.append(native.get("GET", path)[0])
        fetched = len(upstream.calls()) - before
        record = native.record(key_of(url))
        if statuses != [200, 200, 200] or fetched != 2 or record["record"]["fetchCount"] != 3:
            failures.append(f"retention window: {statuses}, {fetched} fetches, {record}")
        print("  byte retention: chunks expire 7 days after their write; a view an hour later "
              "does not extend them, so day 7 + 30 min re-fetches (documented difference)")
    finally:
        native.server.stop()


def pressure(binary, root, upstream, failures):
    """24 distinct cold keys at once: one worker, 16 job slots and a
    16-socket handoff. Readers beyond a free slot get 404 at once with
    nothing recorded; every answered 200 has the exact bytes; afterwards
    every key is fetched exactly once in total."""
    native = Native(binary, root, upstream, "pressure.example=93.184.216.41")
    t = 1_806_100_000_000
    urls = [f"https://pressure.example/{i}.png" for i in range(24)]
    replies = {url: [{"status": 200, "headers": [["Content-Type", "image/png"]],
                      "body": {"kind": "png", "size": 2_000 + i, "seed": i}, "delayMs": 300}]
               for i, url in enumerate(urls)}
    native.start(t)
    try:
        for url in urls:
            native.register(url)
        upstream.set_replies(replies)
        answers = [None] * len(urls)

        def run(index):
            try:
                answers[index] = native.get("GET", f"/v1/media/{key_of(urls[index])}")
            except OSError:
                answers[index] = "reset"
        threads = [threading.Thread(target=run, args=(i,)) for i in range(len(urls))]
        for thread in threads:
            thread.start()
            time.sleep(.01)  # inside the 4 + 8 admission window
        for thread in threads:
            thread.join()
        served = refused = closed = 0
        for index, answer in enumerate(answers):
            if answer in (None, "reset"):
                closed += 1
                continue
            status, _, payload = answer
            expected = body_of(replies[urls[index]][0]["body"], SIGNATURES)
            if status == 200:
                served += 1
                if payload != expected:
                    failures.append(f"pressure: {urls[index]} wrong bytes")
            elif status == 404:
                refused += 1
                record = native.record(key_of(urls[index]))["record"]
                if record["status"] not in ("pending", "ok"):
                    failures.append(f"pressure: a refused read recorded {record['status']}")
            else:
                failures.append(f"pressure: status {status}")
        if served + refused + closed != len(urls) or served < 12:
            failures.append(f"pressure: {served} served, {refused} refused, {closed} closed")
        time.sleep(1.5)  # jobs already queued may still be fetching
        for index, url in enumerate(urls):
            status, _, payload = native.get("GET", f"/v1/media/{key_of(url)}")
            if status != 200 or payload != body_of(replies[url][0]["body"], SIGNATURES):
                failures.append(f"pressure: follow-up {url} -> {status}")
        counts = {}
        for url, _ in upstream.calls():
            if url.startswith("https://pressure.example/"):
                counts[url] = counts.get(url, 0) + 1
        if sorted(counts) != sorted(urls) or set(counts.values()) != {1}:
            failures.append(f"pressure: upstream fetch counts {counts}")
        print(f"  distinct-key pressure: 24 cold keys at once -> {served} served, {refused} refused "
              f"at once (no slot), {closed} closed by admission; every key fetched exactly once")
    finally:
        native.server.stop()


def non_success_header_only(binary, root, upstream, failures):
    """The TypeScript bytes() client never awaits a non-2xx body. A redirect
    and a 503 with stalled bodies must both resolve from headers promptly."""
    native = Native(binary, root, upstream, "headers.example=93.184.216.43")
    t = 1_806_150_000_000
    redirect = "https://headers.example/redirect.png"
    target = "https://headers.example/final.png"
    outage = "https://headers.example/outage.png"
    png = {"status": 200, "body": {"kind": "png", "size": 1000, "seed": 12}}
    native.start(t)
    try:
        for url in (redirect, outage):
            native.register(url)
        upstream.set_replies({
            redirect: [{"status": 302, "headers": [["Location", "/final.png"]],
                        "framing": "declared", "declared": 1000,
                        "stallAfterHeaders": True}],
            target: [png],
            outage: [{"status": 503, "framing": "declared", "declared": 1000,
                      "stallAfterHeaders": True}],
        })
        started = time.monotonic()
        statuses = [native.get("GET", f"/v1/media/{key_of(url)}")[0]
                    for url in (redirect, outage)]
        elapsed = time.monotonic() - started
        state = native.record(key_of(outage))["record"]
        if statuses != [200, 404] or elapsed >= 2 or state["status"] != "failed_transient":
            failures.append(f"non-2xx headers: statuses={statuses}, elapsed={elapsed:.2f}s, "
                            f"state={state['status']}")
        print(f"  non-2xx headers: redirect and 503 returned in {elapsed:.2f}s "
              "without waiting for stalled bodies")
    finally:
        native.server.stop()


def cancellation(binary, root, upstream, failures):
    """SIGTERM while the worker waits on a hung upstream: the transport's
    cancel flag ends the hop within ~50 ms, both waiting readers (the running
    job's and a queued one) get 404, nothing is recorded, and after a restart
    both keys are fetched."""
    native = Native(binary, root, upstream, "hang.example=93.184.216.42")
    t = 1_806_200_000_000
    hung = "https://hang.example/hung.png"
    queued = "https://hang.example/queued.png"
    ok = {"status": 200, "headers": [["Content-Type", "image/png"]],
          "body": {"kind": "png", "size": 1_500, "seed": 5}}
    native.start(t)
    try:
        native.register(hung)
        native.register(queued)
        upstream.set_replies({hung: [{"status": 0, "hang": True}], queued: [ok]})
        answers = {}

        def run(url):
            answers[url] = native.get("GET", f"/v1/media/{key_of(url)}")
        first = threading.Thread(target=run, args=(hung,))
        first.start()
        deadline = time.time() + 5
        while not any(url == hung for url, _ in upstream.calls()) and time.time() < deadline:
            time.sleep(.02)
        second = threading.Thread(target=run, args=(queued,))
        second.start()
        time.sleep(.3)
        started = time.time()
        native.server.process.send_signal(signal.SIGTERM)
        native.server.process.wait(timeout=30)
        exit_seconds = time.time() - started
        native.server.stderr.close()
        first.join(timeout=5)
        second.join(timeout=5)
        statuses = [answers.get(hung) and answers[hung][0], answers.get(queued) and answers[queued][0]]
        if exit_seconds > 3 or statuses != [404, 404]:
            failures.append(f"cancellation: exit after {exit_seconds:.2f} s, readers {statuses}")
        native.start(t + 1000)
        records = [native.record(key_of(url))["record"]["status"] for url in (hung, queued)]
        if records != ["pending", "pending"]:
            failures.append(f"cancellation recorded {records}")
        upstream.set_replies({hung: [ok], queued: [ok]})
        after = [native.get("GET", f"/v1/media/{key_of(url)}")[0] for url in (hung, queued)]
        if after != [200, 200]:
            failures.append(f"cancellation: after restart {after}")
        print(f"  cancellation: SIGTERM during a hung fetch exited in {exit_seconds:.2f} s, both "
              "readers answered 404, nothing recorded, both keys fetched after the restart")
    finally:
        native.server.stop()


SIGNATURES = {}


def main():
    binary = sys.argv[1]
    fixture = harness.load_fixture("media_get_edges.json")
    SIGNATURES.update(fixture["signatures"])
    dns = ",".join(f"{host}={'|'.join(a for a in answers if ':' not in a)}"
                   for host, answers in fixture["dns"].items()
                   if any(":" not in a for a in answers))
    failures = []
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        upstream = Upstream(root, SIGNATURES)
        try:
            native, counted, log = replay(fixture, binary, root / "replay", upstream, dns, failures)
            print(f"  replay: {counted} request steps ({len(EXPECTED)} asserted differences) matched "
                  "status, headers, body, upstream calls and stored records" if not failures else
                  f"  replay: {len(failures)} mismatches")
            privacy(upstream, log, failures)
            orphan_prune(native, fixture, failures)
            retention_window(binary, root / "window", upstream, failures)
            pressure(binary, root / "pressure", upstream, failures)
            non_success_header_only(binary, root / "headers", upstream, failures)
            cancellation(binary, root / "cancel", upstream, failures)
        finally:
            upstream.close()
    if failures:
        print(f"media GET: {len(failures)} failures", file=sys.stderr)
        for failure in failures[:60]:
            print("  " + failure, file=sys.stderr)
        sys.exit(1)
    print("media GET route checks passed")


if __name__ == "__main__":
    main()
