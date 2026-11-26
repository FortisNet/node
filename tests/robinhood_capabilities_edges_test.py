#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/robinhood/sponsored-trades/capabilities
(R10) against tests/corpus/robinhood_capabilities_edges.json,
which the earlier TypeScript implementation's tests
recorded from the real Fastify app over real sockets.

The test binary (build/fortis_node_robinhood_route_test) runs with
FORTIS_NATIVE_ROBINHOOD_CANARY=1 and sends every provider host to one loopback
HTTPS upstream that records each request; the oracle's lowercase development
devices a–d, e (expired) and r (revoked) are seeded by main.cpp.

Checks:
  1. configurations: for every oracle configuration, exact JSON, protobuf
     and HEAD bytes plus every recorded header (rate-limit headers included),
     or, for the values the native loader refuses (inner space, form feed,
     NBSP, non-ASCII, > 255 bytes trimmed, > 4,096 bytes raw), a startup
     refusal naming the variable; no credential in a body or process output;
  2. request edges on the keyless (disabled) server: status, headers and
     exact bytes for Accept forms, HEAD, ignored query/conditional headers,
     malformed authorization, expired and revoked devices and the Origin
     order; asserted router-wide differences: other methods/paths (404 bodies)
     and the percent-encoded segment (native 404, TypeScript 200);
  3. the enabled server's cases, and ATTESTATION_MODE=required refusals;
  4. concurrency: 12 simultaneous identical reads, the oracle's 24-read burst
     (excess connects may be refused at the socket, never answered wrongly)
     and 200 reads from 12 clients;
  5. quota: device d's development minute drained, then the 429s;
  6. SIGTERM and SIGKILL restarts on the same KV serve the same bytes;
  7. no case reached the provider upstream;
  8. production composition (build/fortis_node, development enrollment):
     the canary off answers 404; on, keyless and keyed, it serves the oracle's
     exact disabled and enabled bytes.
Usage: robinhood_capabilities_edges_test.py build/fortis_node_robinhood_route_test [build/fortis_node]
"""
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
from robinhood_route_test import Upstream  # noqa: E402

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE = "robinhood_capabilities_edges.json"
PATH = "/v1/networks/robinhood/sponsored-trades/capabilities"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining",
           "retry-after", "access-control-allow-origin", "etag", "last-modified"]
ROUTER_404 = {"post", "put", "delete", "trailing-slash", "upper-path", "parent-path"}
# TypeScript serves these configurations (enabled); the native loader refuses
# to start, naming the variable (never the value).
REFUSED = {
    "policy-256": "Invalid environment: ALCHEMY_GAS_POLICY_ID is too long",
    "key-256": "Invalid environment: ALCHEMY_API_KEY is too long",
    "key-raw-4097": "Invalid environment: ALCHEMY_API_KEY is too long",
    "policy-inner-space": "Invalid environment: ALCHEMY_GAS_POLICY_ID contains invalid characters",
    "policy-form-feed": "Invalid environment: ALCHEMY_GAS_POLICY_ID contains invalid characters",
    "policy-nbsp": "Invalid environment: ALCHEMY_GAS_POLICY_ID contains invalid characters",
    "policy-non-ascii": "Invalid environment: ALCHEMY_GAS_POLICY_ID contains invalid characters",
    "key-only-blank-policy-nbsp": "Invalid environment: ALCHEMY_GAS_POLICY_ID contains invalid characters",
}
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message)
    return condition


def load_fixture():
    data = (FIXTURES / FIXTURE).read_bytes()
    pinned = (FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


def token(letter):
    return "fintel_dev_" + letter * 43


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def raw(port, method, target, headers, timeout=30):
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers.items()]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
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


def get(port, letter="a", accept=None, method="GET", bearer=None):
    headers = {"authorization": "Bearer " + (bearer or token(letter))}
    if accept:
        headers["accept"] = accept
    return raw(port, method, PATH, headers)


SCRUBBED = ("ALCHEMY_API_KEY", "ALCHEMY_GAS_POLICY_ID", "ATTESTATION_MODE", "NODE_ENV")


class Server:
    def __init__(self, binary, kv_dir, env_extra):
        Path(kv_dir).mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        env = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_") and k not in SCRUBBED}
        env.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(self.port)})
        env.update(env_extra)
        self.process = subprocess.Popen([binary, str(self.port)], env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        for _ in range(400):
            if self.process.poll() is not None:
                self.output = self.process.stdout.read() + self.process.stderr.read()
                raise RuntimeError(self.output.decode("utf-8", "replace"))
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("server did not start")

    def stop(self, sig=signal.SIGTERM):
        self.process.send_signal(sig)
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        return self.process.stdout.read() + self.process.stderr.read()


class Harness:
    def __init__(self, binary, root, fixture):
        self.binary, self.root, self.fixture = binary, root, fixture
        (root / "tls").mkdir()
        self.upstream = Upstream(root / "tls")
        self.upstream.start_case({})

    def server(self, name, env=None):
        return Server(self.binary, self.root / f"kv-{name}", {
            "FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
            "FORTIS_CA_BUNDLE_PATH": str(self.upstream.ca),
            "FORTIS_ENABLE_TEST_ROBINHOOD_ROUTES": "1",
            "FORTIS_TEST_ROBINHOOD_UPSTREAM_PORT": str(self.upstream.port),
            "FORTIS_TEST_ROBINHOOD_UPSTREAM_CA": str(self.upstream.ca),
            **(env or {}),
        })

    def no_secret(self, data, where):
        for secret in self.fixture["secrets"] + ["p" * 64, "k" * 64, "policy with space"]:
            check(secret.encode() not in data, f"{where}: credential {secret[:16]}… leaked")


def wait_for_fresh_minute(budget_seconds):
    """Quota windows are wall-clock minutes: leave room for the whole step."""
    second = time.time() % 60
    if second > 60 - budget_seconds:
        time.sleep(60 - second + 0.2)


def compare(label, expected, actual):
    status, headers, body = expected
    got_status, got_headers, got_body = actual
    problems = []
    if got_status != status:
        problems.append(f"status {got_status} != {status}")
    for name in HEADERS:
        if got_headers.get(name) != headers.get(name):
            problems.append(f"header {name}: {got_headers.get(name)!r} != {headers.get(name)!r}")
    if got_body != body:
        problems.append(f"body {got_body[:80]!r} != {body[:80]!r}")
    return check(not problems, f"{label}: " + "; ".join(problems))


# ---- 1. configurations ----

def configurations(harness, fixture):
    matched = refused = 0
    for config in fixture["configs"]:
        name = config["name"]
        wait_for_fresh_minute(4)
        try:
            server = harness.server(f"config-{name}", config["env"])
        except RuntimeError as error:
            message = str(error)
            check(name in REFUSED and REFUSED[name] in message,
                  f"{name}: TypeScript served enabled={config['enabled']}; native refused: {message[-200:]!r}")
            harness.no_secret(message.encode(), f"{name} refusal output")
            refused += name in REFUSED
            continue
        try:
            if not check(name not in REFUSED, f"{name}: expected a native startup refusal"):
                continue
            json_body = config["responseText"].encode()
            wire = bytes.fromhex(config["wireHex"])
            ok = compare(f"{name} JSON", (200, config["jsonHeaders"], json_body), get(server.port))
            ok = compare(f"{name} protobuf", (200, config["protobufHeaders"], wire),
                         get(server.port, accept="application/protobuf")) and ok
            ok = compare(f"{name} HEAD", (200, config["headHeaders"], b""),
                         get(server.port, accept="application/protobuf", method="HEAD")) and ok
            matched += ok
        finally:
            harness.no_secret(server.stop(), f"{name} output")
    enabled = sum(1 for c in fixture["configs"] if c["enabled"] and c["name"] not in REFUSED)
    print(f"configurations: {matched} byte-identical ({enabled} enabled), {refused} startup refusals "
          f"where TypeScript serves enabled ({len(fixture['configs'])} total)")


# ---- 2/3/5. oracle cases ----

def expected_of(case):
    body = bytes.fromhex(case["wireHex"]) if "wireHex" in case else case["responseText"].encode("utf-8")
    return case["status"], case["headers"], body


def replay_group(port, fixture, group, harness):
    matched = asserted = 0
    for case in fixture["cases"]:
        if case["group"] != group:
            continue
        name = case["name"]
        headers = {}
        if case["device"]:
            headers["authorization"] = "Bearer " + token(case["device"])
        headers.update(case["requestHeaders"])
        target = bytes.fromhex(case["targetHex"]).decode("latin1")
        actual = raw(port, case["method"], target, headers)
        harness.no_secret(actual[2], name)
        if name == "escaped-path":
            check(case["status"] == 200 and actual[0] == 404, f"{name}: expected native 404, got {actual[0]}")
            asserted += 1
        elif name in ROUTER_404:
            check(case["status"] == 404 and actual[0] == 404, f"{name}: status {actual[0]} != 404")
            asserted += 1
        else:
            matched += compare(name, expected_of(case), actual)
    return matched, asserted


# ---- 4. concurrency ----

def concurrency(port, keyless, fixture):
    expected = {"json": keyless["responseText"].encode(), "protobuf": bytes.fromhex(keyless["wireHex"]), "head": b""}
    lengths = {"json": len(expected["json"]), "protobuf": len(expected["protobuf"]), "head": len(expected["json"])}
    oracle = fixture["concurrent"]
    check(oracle["statuses"] == [200] * len(oracle["kinds"]), "oracle concurrent statuses")
    check([bytes.fromhex(h) for h in oracle["payloadHex"]] == [expected[k] for k in oracle["kinds"]],
          "oracle concurrent bodies are the keyless documents")

    def read(kind):
        try:
            return get(port, "b", "application/protobuf" if kind == "protobuf" else None,
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

    def right(kind, result):
        return result[0] == 200 and result[2] == expected[kind] and \
            result[1].get("content-length") == str(lengths[kind]) and result[1].get("vary") == "Accept"

    admitted = oracle["kinds"][:12]
    results = burst(admitted)
    bad = [(admitted[i], r[0]) for i, r in enumerate(results) if not right(admitted[i], r)]
    check(not bad, f"12 simultaneous reads differ: {bad[:5]}")
    results = burst(oracle["kinds"])
    answered = [(kind, r) for kind, r in zip(oracle["kinds"], results) if r[0] != 0]
    wrong = [(kind, r[0]) for kind, r in answered if not right(kind, r)]
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
    bad_more = [(kind, r[0]) for kind, r in outcomes if not right(kind, r)]
    check(len(outcomes) == 200 and not bad_more, f"concurrent clients differ: {bad_more[:5]}")
    print(f"concurrent: 12/12 simultaneous identical; oracle burst of 24: {len(answered)} answered, all identical, "
          f"{24 - len(answered)} refused at the socket; {len(outcomes) - len(bad_more)}/200 from 12 clients identical")


# ---- 8. production composition ----

def production(binary, root, fixture):
    configs = {c["name"]: c for c in fixture["configs"]}
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
    for name in ("keyless", "both"):
        config = configs[name]
        server = Server(binary, root / f"kv-prod-{name}", {"FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca), **config["env"]})
        try:
            body = json.dumps({"platform": "ios", "appVersion": "1.0.0"}).encode()
            with socket.create_connection(("127.0.0.1", server.port), timeout=30) as sock:
                sock.sendall(b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                             b"Content-Type: application/json\r\nContent-Length: " + str(len(body)).encode() +
                             b"\r\n\r\n" + body)
                data = b""
                while chunk := sock.recv(65536):
                    data += chunk
            device = json.loads(data.partition(b"\r\n\r\n")[2])["deviceToken"]
            status, headers, got = get(server.port, bearer=device)
            check(status == 200 and got == config["responseText"].encode() and
                  headers.get("x-rate-limit-remaining") == "239",
                  f"production {name} JSON: {status} {got[:80]!r}")
            status, _, wire = get(server.port, bearer=device, accept="application/protobuf")
            check(status == 200 and wire == bytes.fromhex(config["wireHex"]), f"production {name} protobuf")
        finally:
            output = server.stop()
            for secret in fixture["secrets"]:
                check(secret.encode() not in output, f"production {name}: credential in output")
    print("production: canary unset/0/blank answers 404; keyless and keyed serve the oracle's exact bytes")


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    check(fixture["upstreamFetches"] == 0, "the oracle reached an upstream")
    configs = {c["name"]: c for c in fixture["configs"]}
    keyless = configs["keyless"]
    with tempfile.TemporaryDirectory(prefix="robinhood-capabilities-") as root:
        root = Path(root)
        harness = Harness(binary, root, fixture)
        try:
            configurations(harness, fixture)
            server = harness.server("main")
            try:
                wait_for_fresh_minute(20)
                matched, asserted = replay_group(server.port, fixture, "request", harness)
                print(f"request edges: {matched} matched, {asserted} asserted router differences")
                concurrency(server.port, keyless, fixture)
                wait_for_fresh_minute(25)
                for _ in range(239):
                    status, _, _ = get(server.port, "d", method="HEAD")
                    if not check(status == 200, f"quota drain answered {status}"):
                        break
                matched, _ = replay_group(server.port, fixture, "quota", harness)
                print(f"quota: {matched} matched")
                status, _, before = get(server.port, "c")
                check(status == 200 and before == fixture["restart"]["before"].encode(), "before restart")
            finally:
                harness.no_secret(server.stop(), "main output")
            # Restarts on the same KV directory regenerate the documents from
            # the same configuration. Quota counters are per-minute KV windows
            # natively (TypeScript: process memory), so device d is not
            # compared after a restart inside the drained minute.
            server = harness.server("main")
            status, _, after = get(server.port, "a")
            check(status == 200 and after == fixture["restart"]["after"].encode(), f"after SIGTERM restart: {status}")
            server.stop(signal.SIGKILL)
            server = harness.server("main")
            status, _, after = get(server.port, "b", "application/protobuf")
            check(status == 200 and after == bytes.fromhex(keyless["wireHex"]), f"after SIGKILL restart: {status}")
            harness.no_secret(server.stop(), "restarted output")
            print("restart: SIGTERM and SIGKILL restarts serve the same bytes")

            both = configs["both"]["env"]
            server = harness.server("enabled", both)
            try:
                wait_for_fresh_minute(5)
                matched, _ = replay_group(server.port, fixture, "enabled", harness)
                print(f"enabled: {matched} matched")
            finally:
                harness.no_secret(server.stop(), "enabled output")
            server = harness.server("attestation", {"ATTESTATION_MODE": "required", **both})
            try:
                matched, _ = replay_group(server.port, fixture, "attestation", harness)
                print(f"attestation: {matched} matched")
            finally:
                harness.no_secret(server.stop(), "attestation output")
            calls = harness.upstream.calls()
            check(not calls, f"{len(calls)} request(s) reached the provider upstream: {calls[:2]}")
            print(f"upstream: {len(calls)} provider requests across every case")
        finally:
            harness.upstream.close()
        if len(sys.argv) == 3:
            production(sys.argv[2], root, fixture)
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("robinhood capabilities edges: all checks passed")


if __name__ == "__main__":
    main()
