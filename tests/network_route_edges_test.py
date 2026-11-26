#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks (R09) against
tests/corpus/networks_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets.

The native catalog states what the native process serves, so its capability
values and prices differ from TypeScript's by rule, never by accident. Every
expected native document is TypeScript's document with that substitution,
re-encoded by an encoder that first proves it reproduces every TypeScript
JSON text and protobuf wire body byte for byte:

  address facets (balances, transfers, nfts, insight, txStatus, txDetail,
    popularTokens): TypeScript's value when the address plane is live;
  news: TypeScript's value when the address AND news planes are live;
  token: TypeScript's value when the token plane is live;
  trustlines: always none (no native or TypeScript route serves it);
  prices.spot / prices.history: whether the spot / history plane is live.

Checks:
  1. configurations on the network test binary (no plane live): startup
     refused exactly where loadConfig threw, provider labels, exact bytes for
     JSON and protobuf, no credential in a body or the process output;
  2. request edges: status, headers (including x-rate-limit-remaining) and
     exact bytes for Accept forms, HEAD, ignored query/conditional headers,
     malformed authorization, expired and revoked devices, Origin order;
     asserted router-wide differences: other methods/paths (404 bodies) and
     /v1/%6Eetworks (native 404);
  3. ATTESTATION_MODE=required refusals;
  4. concurrency: 12 simultaneous identical reads, the oracle's 24-read burst
     (excess connects may be refused at the socket, never answered wrongly)
     and 200 reads from 12 clients;
  5. quota: device D's development minute drained, then the 429s;
  6. SIGTERM and SIGKILL restarts serve the same bytes;
  7. live planes: the address-route test binary (address + spot + history
     planes really started) and the token-route test binary (token + spot +
     history) publish exactly the substituted TypeScript documents for the
     keyless and all-keys configurations, and on the address binary every
     advertised address facet is cross-checked against the live route: "none"
     iff the route answers its 400 "… not served for …" refusal.
Usage: network_route_edges_test.py build/fortis_node_network_route_test
       [build/fortis_node_address_route_test build/fortis_node_token_route_test]
"""
import hashlib
import json
import re
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

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE = "networks_edges.json"
PATH = "/v1/networks"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining",
           "retry-after", "access-control-allow-origin", "etag", "last-modified"]
ROUTER_404 = {"post", "put", "delete", "trailing-slash", "upper-path"}
ADDRESS_FACETS = ("balances", "transfers", "nfts", "insight", "txStatus", "txDetail", "popularTokens")
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


# ---- the catalog encoders (protobuf-es and JSON.stringify byte forms) ----

def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def ld(field, payload):
    return varint(field << 3 | 2) + varint(len(payload)) + payload


def vi(field, value):
    return varint(field << 3) + varint(value)


def st(field, text):
    return ld(field, text.encode())


def encode_network(n):
    out = st(1, n["id"]) + st(2, n["family"])
    if "evmChainId" in n:
        out += vi(3, n["evmChainId"])
    if n["testnet"]:
        out += vi(4, 1)
    if "requiredConfirmations" in n:
        out += vi(5, n["requiredConfirmations"])
    for url in n.get("rpcUrls", []):
        out += st(6, url)
    for url in n.get("graphqlUrls", []):
        out += st(7, url)
    if "native" in n:
        native = n["native"]
        body = st(1, native["symbol"])
        if native["decimals"]:
            body += vi(2, native["decimals"])
        if "coingeckoId" in native:
            body += st(3, native["coingeckoId"])
        out += ld(8, body)
    for key, value in n["capabilities"].items():
        out += ld(9, st(1, key) + st(2, value))
    return out


def encode_protobuf(doc):
    out = b"".join(ld(1, encode_network(n)) for n in doc["networks"])
    prices = doc["prices"]
    body = b""
    if prices["spot"]:
        body += vi(1, 1)
    if prices["history"]:
        body += vi(2, 1)
    for key, value in prices["sources"].items():
        body += ld(3, st(1, key) + st(2, value))
    return out + ld(2, body)


def encode_json(doc):
    return json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


class Planes:
    def __init__(self, address=False, token=False, news=False, spot=False, history=False):
        self.address, self.token, self.news, self.spot, self.history = address, token, news, spot, history


def native_doc(ts_doc, planes):
    doc = json.loads(json.dumps(ts_doc))
    for network in doc["networks"]:
        caps = network["capabilities"]
        for key in caps:
            ts = caps[key]
            if key in ADDRESS_FACETS:
                caps[key] = ts if planes.address else "none"
            elif key == "news":
                caps[key] = ts if planes.address and planes.news else "none"
            elif key == "token":
                caps[key] = ts if planes.token else "none"
            else:
                caps[key] = "none"
    doc["prices"]["spot"] = planes.spot
    doc["prices"]["history"] = planes.history
    return doc


def bodies(ts_text, planes):
    doc = native_doc(json.loads(ts_text), planes)
    return encode_json(doc), encode_protobuf(doc)


# ---- sockets and processes ----

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


def get(port, letter="A", accept=None, method="GET", path=PATH):
    headers = {"authorization": "Bearer " + token(letter)}
    if accept:
        headers["accept"] = accept
    return raw(port, method, path, headers)


SCRUBBED = ("COINGECKO_API_KEY", "COINGECKO_TIER", "JUPITER_API_KEY", "ETHERSCAN_API_KEY", "ETHPLORER_API_KEY",
            "TAOSTATS_API_KEY", "TRONSCAN_API_KEY", "TRONGRID_API_KEY", "ALCHEMY_API_KEY", "HELIUS_API_KEY",
            "NEARBLOCKS_API_KEY", "GETBLOCK_DOGE_TOKEN", "GETBLOCK_LTC_TOKEN", "GETBLOCK_ZEC_TOKEN",
            "SAUCERSWAP_API_KEY", "DASH_BLOCKBOOK_URLS", "ATTESTATION_MODE", "NODE_ENV",
            "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "FORTIS_NATIVE_NEWS_CANARY", "FORTIS_NATIVE_ADDRESS_CANARY",
            "FORTIS_NATIVE_CROSS_CHAIN_CANARY", "FORTIS_NATIVE_ROBINHOOD_CANARY", "FORTIS_NATIVE_DASHPAY_CANARY",
            "FORTIS_ENABLE_TEST_NETWORK_ROUTE")


class Server:
    def __init__(self, binary, kv_dir, env_extra, expect_start=True):
        Path(kv_dir).mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        env = dict(os.environ)
        for name in SCRUBBED:
            env.pop(name, None)
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


def network_server(binary, kv_dir, env=None):
    return Server(binary, kv_dir, {"FORTIS_ENABLE_TEST_NETWORK_ROUTE": "1", **(env or {})})


def no_secret(fixture, data, where):
    for secret in fixture["secrets"]:
        check(secret.encode() not in data, f"{where}: credential {secret[:16]}… leaked")


def config_env(fixture, config):
    env = dict(config.get("env", {}))
    for key in config["envKeys"]:
        if key not in env:
            env[key] = fixture["secretEnv"][key]
    return env


def wait_for_fresh_minute(budget_seconds):
    """Quota windows are wall-clock minutes: leave room for the whole run."""
    second = time.time() % 60
    if second > 60 - budget_seconds:
        time.sleep(60 - second + 0.2)


# ---- 0. encoder fidelity ----

def encoder_fidelity(fixture):
    for config in fixture["configs"]:
        if "error" in config:
            continue
        doc = json.loads(config["responseText"])
        check(encode_json(doc) == config["responseText"].encode("utf-8"), f"{config['name']}: JSON encoder")
        check(encode_protobuf(doc).hex() == config["wireHex"], f"{config['name']}: protobuf encoder")
    print("encoder: reproduces every TypeScript JSON text and protobuf wire body")


# ---- 1. configurations ----

def configurations(binary, root, fixture):
    matched = refused = 0
    none = Planes()
    for config in fixture["configs"]:
        name = config["name"]
        env = config_env(fixture, config)
        try:
            server = network_server(binary, root / f"kv-config-{name}", env)
        except RuntimeError as error:
            check("error" in config, f"{name}: TypeScript served this configuration; native refused: "
                                     f"{str(error)[-200:]!r}")
            no_secret(fixture, str(error).encode(), f"{name} refusal output")
            refused += "error" in config
            continue
        try:
            if not check("error" not in config, f"{name}: TypeScript refused ({config.get('error')}); native served"):
                continue
            json_body, proto_body = bodies(config["responseText"], none)
            status, headers, body = get(server.port)
            ok = check(status == 200 and body == json_body, f"{name}: JSON differs: {status} {body[:160]!r}")
            status, headers, wire = get(server.port, accept="application/protobuf")
            ok = check(status == 200 and wire == proto_body, f"{name}: protobuf differs") and ok
            label = json.loads(body)["prices"]["sources"] if body.startswith(b"{") else None
            ok = check(label == config["prices"]["sources"], f"{name}: labels {label}") and ok
            matched += ok
            no_secret(fixture, body + wire, name)
        finally:
            no_secret(fixture, server.stop(), f"{name} output")
    print(f"configurations: {matched} byte-identical (substituted), {refused} refusals matched "
          f"({len(fixture['configs'])} total)")


# ---- 2/3. request edges and attestation ----

def expected_answer(case, keyless_json, keyless_proto):
    """The native answer for an oracle case: TypeScript's, with 200 bodies
    (and their Content-Length) replaced by the substituted documents."""
    status = case["status"]
    headers = dict(case["headers"])
    if "wireHex" in case:
        body = bytes.fromhex(case["wireHex"])
    else:
        body = case["responseText"].encode("utf-8")
    if status == 200:
        proto = headers.get("content-type") == "application/protobuf"
        full = keyless_proto if proto else keyless_json
        headers["content-length"] = str(len(full))
        body = b"" if case["method"] == "HEAD" else full
    return status, headers, body


def compare(case, expected, actual, label=None):
    label = label or case["name"]
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


def replay_group(port, fixture, group, keyless_json, keyless_proto):
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
        if name == "escaped-path":
            check(case["status"] == 200 and actual[0] == 404, f"{name}: expected native 404, got {actual[0]}")
            asserted += 1
        elif name in ROUTER_404:
            check(case["status"] == 404 and actual[0] == 404, f"{name}: status {actual[0]} != 404")
            asserted += 1
        else:
            matched += compare(case, expected_answer(case, keyless_json, keyless_proto), actual)
    return matched, asserted


# ---- 4. concurrency ----

def concurrency(port, keyless_json, keyless_proto, fixture):
    expected = {"json": keyless_json, "protobuf": keyless_proto, "head": b""}
    oracle = fixture["concurrent"]
    check(oracle["statuses"] == [200] * len(oracle["kinds"]), "oracle concurrent statuses")

    def read(kind):
        try:
            return get(port, "B", "application/protobuf" if kind == "protobuf" else None,
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
        if result[0] != 200 or result[2] != expected[kind]:
            return False
        length = len(keyless_proto if kind == "protobuf" else keyless_json) if kind != "head" else len(keyless_json)
        return result[1].get("content-length") == str(length)

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


# ---- 7. live planes ----

def live_binary_env(kind, root, now_ms):
    ca, _, _, _ = tls_ca.create_certificates(root, "DNS:localhost")
    port = str(free_port())  # nothing listens: no case here may reach an upstream
    clock = root / "clock"
    clock.write_text(str(now_ms))
    price = {"FORTIS_ENABLE_TEST_PRICE_ROUTE": "1", "FORTIS_TEST_PRICE_UPSTREAM_PORT": port,
             "FORTIS_TEST_PRICE_UPSTREAM_CA": str(ca), "FORTIS_TEST_PRICE_NOW_FILE": str(clock),
             "FORTIS_TEST_HISTORY_NOW_FILE": str(clock)}
    if kind == "address":
        return {**price, "FORTIS_ENABLE_TEST_ADDRESS_ROUTES": "1", "FORTIS_TEST_ADDRESS_UPSTREAM_PORT": port,
                "FORTIS_TEST_ADDRESS_UPSTREAM_CA": str(ca), "FORTIS_TEST_ADDRESS_NOW_FILE": str(clock)}
    return {**price, "FORTIS_ENABLE_TEST_TOKEN_ROUTES": "1", "FORTIS_TEST_TOKEN_UPSTREAM_PORT": port,
            "FORTIS_TEST_TOKEN_UPSTREAM_CA": str(ca), "FORTIS_TEST_TOKEN_NOW_FILE": str(clock)}


PROBE_ADDRESS = "!"
REFUSALS = {
    "balances": ("/address/{a}/balances", "balances not served for "),
    "transfers": ("/address/{a}/transfers", "transfers not served for "),
    "nfts": ("/address/{a}/nfts", "nfts not served for "),
    "insight": ("/address/{a}/insight", "insight not served for "),
    "txStatus": ("/tx/{h}", "tx status not served for "),
    "txDetail": ("/tx/{h}/view", "no detail view for "),
}


def probe_facets(port, doc):
    """Every advertised address facet against the live route: the facet is
    "none" exactly when the route answers its capability refusal."""
    agreed = 0
    # The address binary seeds premium devices A and B (1200/minute each);
    # alternating them keeps the 258 probes inside one minute's quota.
    for index, network in enumerate(doc["networks"]):
        for facet, (suffix, refusal) in REFUSALS.items():
            path = f"/v1/networks/{network['id']}" + suffix.format(a=PROBE_ADDRESS, h="zz")
            status, _, body = raw(port, "GET", path, {"authorization": "Bearer " + token("AB"[index % 2])})
            refused = status == 400 and refusal.encode() in body
            advertised = network["capabilities"][facet] == "ok"
            if check(advertised != refused, f"{network['id']}.{facet}: advertised {advertised}, route answered "
                                            f"{status} {body[:120]!r}"):
                agreed += 1
    return agreed


def live_planes(address_binary, token_binary, root, fixture):
    configs = {config["name"]: config for config in fixture["configs"]}
    now_ms = int(time.time() * 1000)
    runs = []
    if address_binary:
        runs.append(("address", address_binary, Planes(address=True, spot=True, history=True)))
    if token_binary:
        runs.append(("token", token_binary, Planes(token=True, spot=True, history=True)))
    for kind, binary, planes in runs:
        for name in ("keyless", "all-keys"):
            config = configs[name]
            (root / f"tls-{kind}-{name}").mkdir(exist_ok=True)
            env = {**live_binary_env(kind, root / f"tls-{kind}-{name}", now_ms), **config_env(fixture, config)}
            server = Server(binary, root / f"kv-{kind}-{name}", env)
            try:
                json_body, proto_body = bodies(config["responseText"], planes)
                status, _, body = get(server.port)
                check(status == 200 and body == json_body,
                      f"{kind}/{name}: live document differs: {status} {first_difference(body, json_body)}")
                status, _, wire = get(server.port, accept="application/protobuf")
                check(status == 200 and wire == proto_body, f"{kind}/{name}: live protobuf differs")
                no_secret(fixture, body + wire, f"{kind}/{name}")
                catalog = json.loads(body)["networks"]
                served = sum(v == "ok" for n in catalog for v in n["capabilities"].values())
                landing_status, _, landing_body = raw(server.port, "GET", "/", {})
                check(landing_status == 200, f"{kind}/{name}: landing page failed")
                landing = landing_body.decode()
                for css, capability in (("green", "balances"), ("blue", "transfers"),
                                        ("pink", "token")):
                    match = re.search(r'class="stat-value ' + css + r'">(\d+)</div>', landing)
                    expected = sum(n["capabilities"][capability] != "none" for n in catalog)
                    check(match is not None and int(match.group(1)) == expected,
                          f"{kind}/{name}: landing {capability} count differs from /v1/networks")
                agreed = probe_facets(server.port, json.loads(body)) if kind == "address" else 0
                print(f"live {kind} plane, {name}: substituted TypeScript document byte-identical "
                      f"({served} capabilities ok)" + (f"; {agreed}/{43 * len(REFUSALS)} facets agree with "
                                                         f"the live routes" if kind == "address" else ""))
            finally:
                no_secret(fixture, server.stop(), f"{kind}/{name} output")


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"at {i}: {a[max(0, i - 60):i + 60]!r} vs {b[max(0, i - 60):i + 60]!r}"
    return f"lengths {len(a)} vs {len(b)}"


def main():
    if len(sys.argv) not in (2, 4):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    check(fixture["upstreamFetches"] == 0, "the oracle reached an upstream")
    encoder_fidelity(fixture)
    keyless = next(c for c in fixture["configs"] if c["name"] == "keyless")
    keyless_json, keyless_proto = bodies(keyless["responseText"], Planes())
    with tempfile.TemporaryDirectory(prefix="networks-edges-") as root:
        root = Path(root)
        if os.environ.get("R09_ONLY") != "live":
            configurations(binary, root, fixture)
            server = network_server(binary, root / "kv-main")
            try:
                wait_for_fresh_minute(15)
                matched, asserted = replay_group(server.port, fixture, "request", keyless_json, keyless_proto)
                print(f"request edges: {matched} matched, {asserted} asserted router differences")
                concurrency(server.port, keyless_json, keyless_proto, fixture)
                wait_for_fresh_minute(25)
                for _ in range(239):
                    status, _, _ = get(server.port, "D", method="HEAD")
                    if not check(status == 200, f"quota drain answered {status}"):
                        break
                matched, _ = replay_group(server.port, fixture, "quota", keyless_json, keyless_proto)
                print(f"quota: {matched} matched")
                status, _, before = get(server.port, "C")
                check(status == 200 and before == keyless_json, "before restart")
            finally:
                no_secret(fixture, server.stop(), "main output")
            # Restarts on the same KV directory: the documents are regenerated
            # from the same configuration. Quota counters are per-minute KV
            # windows natively (TypeScript: process memory), so device D is
            # not compared after a restart inside the drained minute.
            server = network_server(binary, root / "kv-main")
            status, _, after = get(server.port, "A")
            check(status == 200 and after == keyless_json, f"after SIGTERM restart: {status}")
            server.stop(signal.SIGKILL)
            server = network_server(binary, root / "kv-main")
            status, _, after = get(server.port, "B", "application/protobuf")
            check(status == 200 and after == keyless_proto, f"after SIGKILL restart: {status}")
            no_secret(fixture, server.stop(), "restarted output")
            print("restart: SIGTERM and SIGKILL restarts serve the same bytes")
            server = network_server(binary, root / "kv-attestation", {"ATTESTATION_MODE": "required"})
            try:
                matched, _ = replay_group(server.port, fixture, "attestation", keyless_json, keyless_proto)
                print(f"attestation: {matched} matched")
            finally:
                server.stop()
        if len(sys.argv) == 4:
            live_planes(sys.argv[2], sys.argv[3], root, fixture)
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("networks route edges: all checks passed")


if __name__ == "__main__":
    main()
