#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/robinhood/stock-tokens (R11) against
tests/corpus/robinhood_stock_catalog_edges.json, which
the earlier TypeScript implementation's tests
recorded from the real Fastify app over real sockets.

The test binary (build/fortis_node_robinhood_route_test) runs with
FORTIS_NATIVE_ROBINHOOD_CANARY=1 and sends api.robinhood.com to one loopback
HTTPS upstream (robinhood_route_test.Upstream) that answers from each case's
provider table with the bytes and delays the TypeScript run served, and
records every request. Per case the replay writes the case's frozen instant
to the Robinhood clock file, so the 15-minute catalog cache expires exactly
where TypeScript's did. Each fixture group is a fresh process with its own KV
directory; `restart-b` reuses `restart-a`'s directory after a SIGKILL.

Checks, per case: the status, every recorded header except
x-rate-limit-remaining (native quota minutes follow the wall clock; compared
in the quota group only), the exact JSON text or protobuf bytes (sha256 and
length beyond 64 KiB) and the provider requests (count, method, URL, accept).
No provider request may carry the client's authorization, cookie,
forwarded-for or user-agent, and no device token or id may reach a response
or process output.

Asserted differences (documented beside the native handler):
  - router: a percent-encoded segment is 404 natively (TypeScript decodes it
    and answers 200); other methods and spellings are 404 in both (status
    only); `stock-tokens/` is the instrument route with an empty address
    (TypeScript 400; asserted as recorded natively, an R12 surface);
  - bounds: a provider body over 512 KiB is a transport failure (502 after
    one request; TypeScript parses it); an answer over 2 MiB is
    500 RESPONSE_TOO_LARGE (TypeScript sends it);
  - symbol order (locale-compare): the ASCII and non-ASCII
    ordering tables have the same instruments (byte-identical rows), sorted
    by ticker in byte order after ASCII lower-casing, then by the ticker's
    own bytes (TypeScript: ICU root collation);
  - every attempt timing out: native stops at its 25 s provider deadline
    (TypeScript at about 31 s); status, body and requests match.

Then: 200 sustained warm reads from 12 clients (every answer the exact
document, or the typed back-pressure 429 when the job pool is full) and the
production composition (build/fortis_node): the canary unset, 0 or blank
answers 404; on, a missing bearer is 401 and an Origin 403 without any
provider request.
Usage: robinhood_stock_catalog_edges_test.py build/fortis_node_robinhood_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tls_ca  # noqa: E402
import robinhood_route_test  # noqa: E402

# Record the headers a leak would use, not only the compared ones.
LEAK_HEADERS = ["authorization", "cookie", "x-forwarded-for", "forwarded", "x-real-ip", "origin", "user-agent"]
robinhood_route_test.UPSTREAM_HEADERS = ["content-type", "accept"] + LEAK_HEADERS
Upstream = robinhood_route_test.Upstream

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE = "robinhood_stock_catalog_edges.json"
PATH = "/v1/networks/robinhood/stock-tokens"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining",
           "retry-after", "access-control-allow-origin", "etag", "last-modified"]
ROUTER_404 = {"post", "upper-path"}
TOO_LARGE = b'{"error":"RESPONSE_TOO_LARGE"}'
UNAVAILABLE = b'{"error":"UPSTREAM_UNAVAILABLE"}'
FAILURES = []
TIMINGS = {}


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)
    return condition


def load_fixture():
    data = (FIXTURES / FIXTURE).read_bytes()
    pinned = (FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    import strict_contract_expectations
    return strict_contract_expectations.apply("robinhood_stock_catalog_edges.json", json.loads(data))


def token(letter):
    return "fintel_dev_" + letter * 43


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def raw(port, method, target, headers, timeout=60):
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers.items()]
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


class Server:
    def __init__(self, binary, kv_dir, env_extra):
        Path(kv_dir).mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FORTIS_") and k not in ("ALCHEMY_API_KEY", "ALCHEMY_GAS_POLICY_ID",
                                                            "ATTESTATION_MODE", "NODE_ENV")}
        env.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(self.port)})
        env.update(env_extra)
        self.process = subprocess.Popen([binary, str(self.port)], env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        for _ in range(400):
            if self.process.poll() is not None:
                raise RuntimeError((self.process.stdout.read() + self.process.stderr.read()).decode("utf-8", "replace"))
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("server did not start")

    def stop(self, sig=signal.SIGTERM):
        self.process.send_signal(sig)
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        return self.process.stdout.read() + self.process.stderr.read()


def resolve_table(providers, bodies):
    table = {}
    for key, entries in providers.items():
        table[key] = [dict(entry, body=bodies[entry["bodySha256"]]) if "bodySha256" in entry else entry
                      for entry in entries]
    return table


def expected_body(record):
    if "wireHex" in record:
        return bytes.fromhex(record["wireHex"])
    if "responseText" in record:
        return record["responseText"].encode("utf-8")
    return None


def compare(label, record, actual, remaining=False):
    status, headers, payload = actual
    problems = []
    if status != record["status"]:
        problems.append(f"status {status} != {record['status']}")
    for name in HEADERS:
        if name == "x-rate-limit-remaining" and not remaining:
            continue
        if headers.get(name) != record["headers"].get(name):
            problems.append(f"header {name}: {headers.get(name)!r} != {record['headers'].get(name)!r}")
    want = expected_body(record)
    if want is not None:
        if payload != want:
            at = next((i for i in range(min(len(payload), len(want))) if payload[i] != want[i]),
                      min(len(payload), len(want)))
            problems.append(f"body differs at {at}: {payload[max(0, at - 60):at + 60]!r} != "
                            f"{want[max(0, at - 60):at + 60]!r}")
    elif len(payload) != record["bodyLength"] or hashlib.sha256(payload).hexdigest() != record["bodySha256"]:
        problems.append(f"body length/sha256 {len(payload)} != {record['bodyLength']}")
    return check(not problems, f"{label}: " + "; ".join(problems))


def compare_upstream(label, case, observed, secrets):
    expected = case["upstream"]
    want = collections.Counter((c["method"], c["url"]) for c in expected)
    got = collections.Counter((c["method"], c["url"]) for c in observed)
    ok = check(want == got, f"{label}: provider requests {sorted(got.items())} != {sorted(want.items())}")
    for call in observed:
        headers = call["headers"]
        ok = check((headers.get("accept") or "").lower() == "application/json",
                   f"{label}: provider accept {headers.get('accept')!r}") and ok
        for name in ("authorization", "cookie", "x-forwarded-for", "forwarded", "x-real-ip", "origin"):
            ok = check(name not in headers, f"{label}: provider request carries {name}") and ok
        text = json.dumps(call)
        for secret in secrets:
            ok = check(secret not in text, f"{label}: a client secret reached the provider") and ok
    return ok


def protobuf_rows(payload):
    """The length-delimited field-1 entries of a RobinhoodStockCatalogResponse."""
    rows, rest, position = [], [], 0
    while position < len(payload):
        tag = payload[position]
        position += 1
        length = shift = 0
        while True:
            byte = payload[position]
            position += 1
            length |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        (rows if tag == 0x0A else rest).append(payload[position:position + length])
        position += length
    return rows, rest


class Harness:
    def __init__(self, binary, root, fixture):
        self.binary, self.root, self.fixture = binary, root, fixture
        (root / "tls").mkdir()
        self.upstream = Upstream(root / "tls")
        self.clock = root / "clock"
        self.tickets = root / "tickets"
        self.tickets.write_text("0")
        self.secrets = [token(letter) for letter in "abcder"] + list(fixture["deviceIds"].values()) + \
            list(fixture["clientHeaders"].values())

    def server(self, kv_name, env, at):
        self.clock.write_text(str(at))
        return Server(self.binary, self.root / f"kv-{kv_name}", {
            "FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
            "FORTIS_CA_BUNDLE_PATH": str(self.upstream.ca),
            "FORTIS_ENABLE_TEST_ROBINHOOD_ROUTES": "1",
            "FORTIS_TEST_ROBINHOOD_UPSTREAM_PORT": str(self.upstream.port),
            "FORTIS_TEST_ROBINHOOD_UPSTREAM_CA": str(self.upstream.ca),
            "FORTIS_TEST_ROBINHOOD_NOW_FILE": str(self.clock),
            "FORTIS_TEST_ROBINHOOD_TICKET_FILE": str(self.tickets),
            **env,
        })

    def no_secret(self, data, where):
        for secret in self.secrets:
            check(secret.encode() not in data, f"{where}: {secret[:18]}… reached a response or the output")


def headers_of(case, kind=None):
    headers = {}
    if case["device"]:
        headers["authorization"] = "Bearer " + token(case["device"])
    headers.update(case["requestHeaders"])
    if kind == "protobuf":
        headers["accept"] = "application/protobuf"
    return headers


def run_concurrent(harness, port, case, target):
    kinds = case["concurrent"]
    results = [None] * len(kinds)
    barrier = threading.Barrier(len(kinds))

    def one(index, kind):
        barrier.wait()
        try:
            results[index] = raw(port, "HEAD" if kind == "head" else "GET", target, headers_of(case, kind))
        except (OSError, IndexError, ValueError) as error:
            results[index] = (0, {}, repr(error).encode())

    threads = [threading.Thread(target=one, args=(i, kind)) for i, kind in enumerate(kinds)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    matched = 0
    for index, (record, actual) in enumerate(zip(case["responses"], results)):
        harness.no_secret(actual[2], case["name"])
        matched += compare(f"{case['name']}[{index}:{kinds[index]}]", record, actual)
    return matched == len(kinds)


def replay_case(harness, port, case):
    """Returns 'matched', 'asserted' or 'failed'."""
    name = case["name"]
    harness.clock.write_text(str(case["at"]))
    harness.upstream.start_case(resolve_table(case["providers"], harness.fixture["bodies"]))
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    started = time.monotonic()
    if "concurrent" in case:
        ok = run_concurrent(harness, port, case, target)
        elapsed = time.monotonic() - started
        ok = compare_upstream(name, case, harness.upstream.calls(), harness.secrets) and ok
        TIMINGS[name] = (elapsed, case["elapsedMs"] / 1000)
        return "matched" if ok else "failed"
    actual = raw(port, case["method"], target, headers_of(case))
    elapsed = time.monotonic() - started
    TIMINGS[name] = (elapsed, case["elapsedMs"] / 1000)
    harness.no_secret(actual[2], name)
    observed = harness.upstream.calls()
    status, headers, payload = actual
    if name == "escaped-path":
        check(case["status"] == 200 and status == 404 and not observed, f"{name}: native {status}, expected 404")
        return "asserted"
    if name in ROUTER_404:
        check(case["status"] == 404 and status == 404 and not observed, f"{name}: status {status} != 404")
        return "asserted"
    if name == "trailing-slash":
        # The instrument route with an empty address: TypeScript's zod 400.
        check(case["status"] == 400 and status in (400, 404) and not observed,
              f"{name}: native {status} (TypeScript 400)")
        print(f"  trailing-slash: TypeScript 400, native {status} {payload[:60]!r}", flush=True)
        return "asserted" if status != 400 else ("matched" if compare(name, case, actual) else "failed")
    if name == "over-cap-json":
        check(case["status"] == 200 and status == 502 and payload == UNAVAILABLE and headers.get("vary") is None,
              f"{name}: native {status} {payload[:60]!r}, expected 502 UPSTREAM_UNAVAILABLE")
        # A declared length over the cap fails the attempt before the body
        # is read and is not retried.
        check(len(observed) == 1, f"{name}: {len(observed)} provider requests, expected 1")
        return "asserted"
    if name.startswith("expansion-"):
        check(case["status"] == 200 and status == 500 and payload == TOO_LARGE and
              headers.get("x-rate-limit-cost") == "1",
              f"{name}: native {status} {payload[:60]!r}, expected 500 RESPONSE_TOO_LARGE")
        check(len(observed) == (1 if name == "expansion-json" else 0), f"{name}: {len(observed)} provider requests")
        return "asserted"
    if name.startswith(("unicode-", "ascii-")):
        # locale-compare: the ordering tables are the oracle's rows
        # in ICU order; this server sorts them by ticker in byte order after
        # ASCII lower-casing, then by the ticker's own bytes.
        ok = check(status == case["status"] == 200, f"{name}: status {status}")
        want = expected_body(case)
        if name.endswith("json"):
            mine, theirs = json.loads(payload), json.loads(want)
            key = lambda row: json.dumps(row, sort_keys=True)  # noqa: E731
            ok = check(sorted(map(key, mine["assets"])) == sorted(map(key, theirs["assets"])) and
                       mine["source"] == theirs["source"], f"{name}: the instruments differ") and ok
            order = [row["tokenSymbol"] for row in mine["assets"]]
            ok = check(order == sorted(order, key=symbol_order), f"{name}: rows not in symbol order") and ok
        else:
            mine, theirs = protobuf_rows(payload), protobuf_rows(want)
            ok = check(sorted(mine[0]) == sorted(theirs[0]) and mine[1] == theirs[1],
                       f"{name}: protobuf rows differ") and ok
        ok = check(len(payload) == len(want), f"{name}: length {len(payload)} != {len(want)}") and ok
        ok = compare_upstream(name, case, observed, harness.secrets) and ok
        return "asserted" if ok else "failed"
    ok = compare(name, case, actual, remaining=case["group"] == "quota")
    ok = compare_upstream(name, case, observed, harness.secrets) and ok
    if name == "timeout-every-attempt":
        check(elapsed < 27, f"{name}: native took {elapsed:.1f} s (deadline 25 s)")
    return "matched" if ok else "failed"


def symbol_order(symbol):
    """RobinhoodSymbolCompare as a sort key: ASCII-folded bytes, then bytes."""
    raw = symbol.encode("utf-8")
    return raw.translate(bytes.maketrans(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ", b"abcdefghijklmnopqrstuvwxyz")), raw


def wait_for_fresh_minute(budget_seconds):
    second = time.time() % 60
    if second > 60 - budget_seconds:
        time.sleep(60 - second + 0.2)


def sustained(port, document, protobuf_document):
    kinds = [("json", "protobuf", "head")[i % 3] for i in range(200)]
    expected = {"json": document, "protobuf": protobuf_document, "head": b""}
    lock = threading.Lock()
    outcomes = []

    def client(offset):
        for kind in kinds[offset::12]:
            headers = {"authorization": "Bearer " + token("b")}
            if kind == "protobuf":
                headers["accept"] = "application/protobuf"
            try:
                result = raw(port, "HEAD" if kind == "head" else "GET", PATH, headers)
            except (OSError, IndexError, ValueError) as error:
                result = (0, {}, repr(error).encode())
            with lock:
                outcomes.append((kind, result))

    threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    exact = sum(1 for kind, r in outcomes if r[0] == 200 and r[2] == expected[kind])
    pressure = sum(1 for kind, r in outcomes if r[0] == 429 and r[1].get("retry-after") == "2" and
                   (kind == "head" or b'"RATE_LIMITED"' in r[2]))
    refused = sum(1 for _, r in outcomes if r[0] == 0)
    wrong = len(outcomes) - exact - pressure - refused
    check(wrong == 0 and len(outcomes) == 200 and exact >= 150,
          f"sustained: {exact} exact, {pressure} back-pressure 429, {refused} refused, {wrong} wrong")
    print(f"sustained: {exact}/200 exact documents from 12 clients, {pressure} typed back-pressure 429, "
          f"{refused} refused at the socket, {wrong} wrong", flush=True)


def production(binary, root):
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    for value in (None, "0", ""):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ROBINHOOD_CANARY"] = value
        server = Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            status, _, _ = raw(server.port, "GET", PATH, {"authorization": "Bearer " + token("a")})
            check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            server.stop()
    server = Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
                                                   "FORTIS_CA_BUNDLE_PATH": str(ca)})
    try:
        status, headers, body = raw(server.port, "GET", PATH, {})
        check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
              f"production no-auth: {status} {body!r}")
        status, _, body = raw(server.port, "GET", PATH, {"origin": "https://evil.example"})
        check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
    finally:
        server.stop()
    print("production: canary unset/0/blank answers 404; on, no bearer 401 and Origin 403 (no provider request)")


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    documents = {d["name"]: d for d in fixture["documents"]}
    with tempfile.TemporaryDirectory(prefix="robinhood-stock-catalog-") as root:
        root = Path(root)
        harness = Harness(binary, root, fixture)
        try:
            for name, cases in by_group.items():
                group = groups[name]
                kv_name = "restart" if name.startswith("restart-") else name
                server = harness.server(kv_name, group["env"], group["start"])
                try:
                    if name == "quota":
                        wait_for_fresh_minute(20)
                        harness.upstream.start_case({})
                        for _ in range(240):
                            status, _, _ = raw(server.port, "GET",
                                               "/v1/networks/robinhood/sponsored-trades/capabilities",
                                               {"authorization": "Bearer " + token("d")})
                            if not check(status == 200, f"quota drain answered {status}"):
                                break
                    results = collections.Counter(replay_case(harness, server.port, case) for case in cases)
                    totals.update(results)
                    print(f"{name}: {results['matched']} matched, {results['asserted']} asserted differences, "
                          f"{results['failed']} failed", flush=True)
                    if name == "main":
                        harness.clock.write_text(str(cases[-1]["at"]))
                        harness.upstream.start_case({})
                        doc_b = documents["B"]
                        sustained(server.port, doc_b["json"]["text"].encode(), bytes.fromhex(doc_b["protobuf"]["hex"]))
                        check(not harness.upstream.calls(), "sustained warm reads reached the provider")
                finally:
                    output = server.stop(signal.SIGKILL if name == "restart-a" else signal.SIGTERM)
                    harness.no_secret(output, f"{name} output")
        finally:
            harness.upstream.close()
        if len(sys.argv) == 3:
            production(sys.argv[2], root)
    slow = {k: v for k, v in TIMINGS.items() if v[0] > 1 or v[1] > 1}
    for name, (native, oracle) in sorted(slow.items()):
        print(f"  timing {name}: native {native:.2f} s, TypeScript {oracle:.2f} s")
    print(f"robinhood stock catalog: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("robinhood stock catalog edges: all checks passed")


if __name__ == "__main__":
    main()
