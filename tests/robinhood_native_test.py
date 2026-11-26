#!/usr/bin/env python3
"""Native-only behaviour of the Robinhood routes that the frozen
TypeScript replay (robinhood_route_test.py) does not reach.

Production composition (build/fortis_node, development enrollment):
  - FORTIS_NATIVE_ROBINHOOD_CANARY unset/0/blank leaves the six routes absent
    (404 where the canary answers 401); an invalid canary value, a missing CA
    bundle and an ALCHEMY_GAS_POLICY_ID with invalid characters stop startup;
  - without ALCHEMY_API_KEY the capabilities report the disabled reason and
    diagnose, prepare and submit answer their 503s at once (no upstream);
    with the key and the gas policy the capabilities report EIP-7702
    sponsorship; neither secret reaches process output.

Test composition (build/fortis_node_robinhood_route_test, one loopback
upstream for api.robinhood.com and both Alchemy hosts):
  - the 1,000 pending-ticket cap at the route: 1,000 prepares succeed (spread
    over twenty premium fixture devices), the next answers 503, and once the
    tickets expire (the test clock moves two minutes) prepare succeeds again;
  - concurrent submits of one ticket: exactly one reaches Alchemy and answers
    200, every other one answers 404, and the spent ticket stays spent;
  - the provider deadline (shortened by FORTIS_TEST_ROBINHOOD_DEADLINE_MS): a
    stalled catalog is a 502, a stalled simulation a 500, a stalled submit a
    500 that leaves the ticket claimable (a later submit succeeds once);
  - saturation: twelve concurrent slow diagnoses are all answered, 200 or the
    typed 429 back-pressure, never 5xx;
  - a SIGKILL + restart drops every ticket (submit answers 404) and refetches
    the catalog;
  - the Alchemy key appears only in upstream URL paths and the gas policy id
    only in wallet_prepareCalls bodies: never in a response or process output.
Usage: robinhood_native_test.py <fortis_node> <fortis_node_robinhood_route_test>
"""
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

import tls_ca
from robinhood_route_test import Server, Upstream, free_port

FAILURES = []
KEY = "native-alchemy-secret-123456"
POLICY = "native-gas-policy-123456"
SECRETS = {"ALCHEMY_API_KEY": KEY, "ALCHEMY_GAS_POLICY_ID": POLICY}
SENDER = "0x" + "22" * 20
TSLA = "0x" + "11" * 20
USDG = "0x5fc5360d0400a0fd4f2af552add042d716f1d168"
ROUTER = "0x89e5db8b5aa49aa85ac63f691524311aeb649eba"
AMOUNT = 1_000_000


def word(value):
    return format(value, "064x")


def address_word(address):
    return "0" * 24 + address[2:]


# swapExactTokensForTokensSupportingFeeOnTransferTokens(amountIn, 1, [USDG, TSLA], SENDER, deadline)
SWAP = ("0x5c11d795" + word(AMOUNT) + word(1) + word(0xa0) + address_word(SENDER) + word(2_000_000_000) + word(2)
        + address_word(USDG) + address_word(TSLA))
APPROVE = "0x095ea7b3" + address_word(ROUTER) + word(AMOUNT)
CALLS = [{"to": USDG, "data": APPROVE}, {"to": ROUTER, "data": SWAP}]
TRADE = {"from": SENDER, "calls": CALLS}
SIGNATURE = "0x" + "33" * 65
ASSETS = {"assets": [{"id": "0x" + "ab" * 32, "tokenSymbol": "TSLA", "tokenName": "Tesla Stock Token",
                      "deployments": [{"chainId": 4663, "contractAddress": TSLA}], "currentMultiplier": "1",
                      "pendingMultiplier": "", "status": "ASSET_STATUS_ACTIVE"}]}
PREPARED = {"type": "user-operation-v070", "chainId": "0x1237", "data": {"sender": SENDER},
            "signatureRequest": {"type": "personal_sign", "rawPayload": "0x" + "bb" * 32}}
SIM_KEY = f"POST robinhood-mainnet.g.alchemy.com/v2/{KEY}"
WALLET_KEY = f"POST api.g.alchemy.com/v2/{KEY}"
SIMULATION = {"jsonrpc": "2.0", "id": 1, "result": [{"calls": [{"status": "0x1", "gasUsed": "0x1"},
                                                                 {"status": "0x1", "gasUsed": "0x2"}]}]}


def reply(document, delay_ms=0, status=200):
    entry = {"status": status, "body": json.dumps(document)}
    if delay_ms:
        entry["delayMs"] = delay_ms
    return [entry]


def table(assets_delay=0, simulate_delay=0, send_delay=0):
    return {
        "GET api.robinhood.com/rhj/assets": reply(ASSETS, assets_delay),
        f"{SIM_KEY} eth_simulateV1": reply(SIMULATION, simulate_delay),
        f"{SIM_KEY} debug_traceCallMany": reply({"jsonrpc": "2.0", "id": 1, "result": [[]]}),
        f"{WALLET_KEY} wallet_prepareCalls": reply({"jsonrpc": "2.0", "id": 1, "result": PREPARED}),
        f"{WALLET_KEY} wallet_sendPreparedCalls": reply({"jsonrpc": "2.0", "id": 1, "result": {"id": "0x01"}}, send_delay),
    }


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message)


def http(port, method, path, token=None, body=None, timeout=60):
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    data = b""
    if token:
        lines.append(f"Authorization: Bearer {token}")
    if body is not None:
        data = json.dumps(body).encode()
        lines += ["Content-Type: application/json", f"Content-Length: {len(data)}"]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + data)
        raw = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw.extend(chunk)
    head, _, payload = bytes(raw).partition(b"\r\n\r\n")
    first, *rest = head.decode("latin1").split("\r\n")
    headers = {line.split(":", 1)[0].lower(): line.split(":", 1)[1].strip() for line in rest if ":" in line}
    return int(first.split()[1]), headers, payload


def token(letter):
    return "fintel_dev_" + letter * 43


class Production:
    def __init__(self, binary, root, env):
        self.port = free_port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_") and k not in SECRETS}
        environment.update({"FORTIS_KV_DIR": str(root / "kv"), "PORT": str(self.port)})
        environment.update(env)
        (root / "kv").mkdir(parents=True, exist_ok=True)
        self.process = subprocess.Popen([binary, str(self.port)], env=environment, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.started = False
        for _ in range(300):
            if self.process.poll() is not None:
                return
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    self.started = True
                    return
            except OSError:
                time.sleep(.03)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return (self.process.stdout.read() + self.process.stderr.read()).decode("utf-8", "replace")


def enroll(port):
    status, _, body = http(port, "POST", "/v1/devices", body={"platform": "ios", "appVersion": "1.0.0"})
    check(status == 201, f"development enrollment answers {status}")
    return json.loads(body)["deviceToken"]


ROUTES = (("GET", "/v1/networks/robinhood/stock-tokens"), ("GET", f"/v1/networks/robinhood/stock-tokens/{TSLA}"),
          ("GET", "/v1/networks/robinhood/sponsored-trades/capabilities"),
          ("POST", "/v1/networks/robinhood/transactions/diagnose"),
          ("POST", "/v1/networks/robinhood/sponsored-trades/prepare"),
          ("POST", "/v1/networks/robinhood/sponsored-trades/submit"))


def production_checks(binary, root, ca):
    for value in (None, "0", ""):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ROBINHOOD_CANARY"] = value
        process = Production(binary, root / f"off-{value}", env)
        check(process.started, f"canary {value!r}: process starts")
        if process.started:
            for method, path in ROUTES:
                status, _, _ = http(process.port, method, path, body={} if method == "POST" else None)
                check(status == 404, f"canary {value!r}: {path} answers {status}, not 404")
        process.stop()
    refusals = [
        ({"FORTIS_NATIVE_ROBINHOOD_CANARY": "2", "FORTIS_CA_BUNDLE_PATH": str(ca)}, "must be 0 or 1"),
        ({"FORTIS_NATIVE_ROBINHOOD_CANARY": "1"}, "FORTIS_NATIVE_ROBINHOOD_CANARY=1 requires FORTIS_CA_BUNDLE_PATH"),
        ({"FORTIS_NATIVE_ROBINHOOD_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
          "ALCHEMY_GAS_POLICY_ID": "policy with space"}, "ALCHEMY_GAS_POLICY_ID contains invalid characters"),
    ]
    for index, (env, message) in enumerate(refusals):
        process = Production(binary, root / f"refuse-{index}", env)
        output = process.stop()
        check(not process.started and message in output, f"refusal {index}: {message!r} not in {output[-200:]!r}")
    # On without Alchemy: the honest disabled answers, at once.
    process = Production(binary, root / "on", {"FORTIS_NATIVE_ROBINHOOD_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca)})
    check(process.started, "canary on: process starts")
    if process.started:
        for method, path in ROUTES:
            status, _, _ = http(process.port, method, path, body={} if method == "POST" else None)
            check(status in (400, 401) if method == "POST" else status == 401,
                  f"canary on: {path} without a bearer answers {status}")
        device = enroll(process.port)
        status, _, body = http(process.port, "GET", "/v1/networks/robinhood/sponsored-trades/capabilities", device)
        capabilities = json.loads(body)
        check(status == 200 and capabilities["enabled"] is False and
              capabilities["reason"] == "ALCHEMY_GAS_POLICY_NOT_CONFIGURED", f"disabled capabilities: {body!r}")
        started = time.monotonic()
        for path, expected in (("/v1/networks/robinhood/transactions/diagnose", "TRANSACTION_DIAGNOSTICS_UNAVAILABLE"),
                               ("/v1/networks/robinhood/sponsored-trades/prepare", "SPONSORED_TRADES_UNAVAILABLE")):
            status, _, body = http(process.port, "POST", path, device, TRADE)
            check(status == 503 and json.loads(body)["error"] == expected, f"{path} unconfigured: {status} {body!r}")
        status, _, body = http(process.port, "POST", "/v1/networks/robinhood/sponsored-trades/submit", device,
                               {"ticket": "x" * 32, "signatures": [SIGNATURE]})
        check(status == 503, f"submit unconfigured: {status} {body!r}")
        check(time.monotonic() - started < 2, "unconfigured answers reached no upstream")
    process.stop()
    process = Production(binary, root / "keyed", dict(SECRETS, FORTIS_NATIVE_ROBINHOOD_CANARY="1",
                                                      FORTIS_CA_BUNDLE_PATH=str(ca)))
    check(process.started, "keyed: process starts")
    if process.started:
        device = enroll(process.port)
        status, _, body = http(process.port, "GET", "/v1/networks/robinhood/sponsored-trades/capabilities", device)
        capabilities = json.loads(body)
        check(status == 200 and capabilities["enabled"] is True and "reason" not in capabilities
              and capabilities["accountStandard"] == "eip-7702" and capabilities["chainId"] == 4663,
              f"keyed capabilities: {body!r}")
    output = process.stop()
    check(KEY not in output and POLICY not in output, "a secret reached process output")


def prepare(port, letter, timeout=60):
    return http(port, "POST", "/v1/networks/robinhood/sponsored-trades/prepare", token(letter), TRADE, timeout=timeout)


def submit(port, letter, ticket, timeout=60):
    return http(port, "POST", "/v1/networks/robinhood/sponsored-trades/submit", token(letter),
                {"ticket": ticket, "signatures": [SIGNATURE]}, timeout=timeout)


def test_binary_checks(binary, root):
    (root / "tls").mkdir()
    upstream = Upstream(root / "tls")
    clock = root / "clock"
    tickets = root / "tickets"
    base = 1_800_000_000_000
    clock.write_text(str(base))
    tickets.write_text("0")
    try:
        # The pending-ticket cap.
        (root / "kv-cap").mkdir()
        server = Server(binary, upstream, root / "kv-cap", clock, tickets, dict(SECRETS))
        upstream.start_case(table())
        statuses = []
        lock = threading.Lock()
        letters = [chr(ord("A") + i) for i in range(20)]

        def fill(worker):
            for n in range(250):
                status, _, _ = prepare(server.port, letters[(worker * 5 + n) % 20])
                with lock:
                    statuses.append(status)

        threads = [threading.Thread(target=fill, args=(w,)) for w in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        check(statuses.count(200) == 1000, f"cap: {statuses.count(200)} of 1000 prepares succeeded "
                                           f"({sorted(set(statuses))})")
        status, _, body = prepare(server.port, "A")
        check(status == 503 and json.loads(body)["error"] == "SPONSORED_TRADES_UNAVAILABLE",
              f"cap: the 1,001st prepare answers {status} {body!r}")
        clock.write_text(str(base + 120_000))
        status, _, body = prepare(server.port, "B")
        check(status == 200, f"cap: prepare after expiry answers {status} {body[:120]!r}")
        server.stop()

        # Concurrent submits of one ticket.
        (root / "kv-concurrent").mkdir()
        server = Server(binary, upstream, root / "kv-concurrent", clock, tickets, dict(SECRETS))
        upstream.start_case(table(send_delay=500))
        status, _, body = prepare(server.port, "C")
        ticket = json.loads(body)["ticket"]
        results = []

        def race():
            try:
                outcome = submit(server.port, "C", ticket)
            except OSError as error:
                outcome = (-1, {}, str(error).encode())
            with lock:
                results.append(outcome[0])

        threads = [threading.Thread(target=race) for _ in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        sends = [c for c in upstream.calls() if c["key"].endswith("wallet_sendPreparedCalls")]
        check(sorted(results) == [200] + [404] * 7 and len(sends) == 1,
              f"concurrent submits: {sorted(results)}, {len(sends)} sends")
        status, _, _ = submit(server.port, "C", ticket)
        check(status == 404, f"spent ticket answers {status}")

        # Deadline: stalled reads and sends answer inside the handoff.
        output = server.stop()
        (root / "kv-deadline").mkdir()
        server = Server(binary, upstream, root / "kv-deadline", clock, tickets,
                        dict(SECRETS, FORTIS_TEST_ROBINHOOD_DEADLINE_MS="1500"))
        upstream.start_case(table(assets_delay=4000))
        started = time.monotonic()
        status, _, body = http(server.port, "GET", "/v1/networks/robinhood/stock-tokens", token("D"))
        check(status == 502 and time.monotonic() - started < 3.5, f"stalled catalog: {status} {body[:80]!r}")
        time.sleep(3)
        upstream.start_case(table(simulate_delay=4000))
        started = time.monotonic()
        status, _, body = http(server.port, "POST", "/v1/networks/robinhood/transactions/diagnose", token("D"), TRADE)
        check(status == 500 and time.monotonic() - started < 3.5, f"stalled simulation: {status} {body[:80]!r}")
        time.sleep(3)
        upstream.start_case(table(send_delay=4000))
        status, _, body = prepare(server.port, "E")
        ticket = json.loads(body)["ticket"] if status == 200 else ""
        started = time.monotonic()
        status, _, body = submit(server.port, "E", ticket)
        check(status == 500 and time.monotonic() - started < 3.5, f"stalled submit: {status} {body[:80]!r}")
        time.sleep(3)
        upstream.start_case(table())
        status, _, _ = submit(server.port, "E", ticket)
        check(status == 200, f"submit after a failed send answers {status}")
        status, _, _ = submit(server.port, "E", ticket)
        check(status == 404, f"the delivered ticket is spent ({status})")
        output += server.stop()

        # Saturation: answered 200 or the typed 429.
        (root / "kv-load").mkdir()
        server = Server(binary, upstream, root / "kv-load", clock, tickets, dict(SECRETS))
        upstream.start_case(table(simulate_delay=1500))
        loads = []

        def diagnose(index):
            try:
                status, headers, body = http(server.port, "POST", "/v1/networks/robinhood/transactions/diagnose",
                                             token(letters[index]), TRADE, timeout=45)
            except OSError as error:
                status, headers, body = -1, {}, str(error).encode()
            with lock:
                loads.append((status, headers.get("retry-after"), body))

        threads = [threading.Thread(target=diagnose, args=(i,)) for i in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        load_statuses = sorted(r[0] for r in loads)
        check(all(s in (200, 429) for s in load_statuses) and 200 in load_statuses,
              f"saturation statuses: {load_statuses}")
        for status, retry, body in loads:
            if status == 429:
                check(retry is not None and json.loads(body)["error"] == "RATE_LIMITED", f"429 shape: {body[:80]!r}")
        print(f"saturation: {load_statuses.count(200)} x 200, {load_statuses.count(429)} x 429")

        # SIGKILL + restart: tickets are process memory.
        upstream.start_case(table())
        status, _, body = prepare(server.port, "F")
        ticket = json.loads(body)["ticket"] if status == 200 else ""
        check(status == 200, f"prepare before restart: {status}")
        server.process.send_signal(signal.SIGKILL)
        server.process.wait()
        output += server.process.stdout.read() + server.process.stderr.read()
        server = Server(binary, upstream, root / "kv-load", clock, tickets, dict(SECRETS))
        upstream.start_case(table())
        status, _, body = submit(server.port, "F", ticket)
        check(status == 404, f"submit after restart: {status} {body!r}")
        status, _, body = http(server.port, "GET", "/v1/networks/robinhood/stock-tokens", token("F"))
        check(status == 200 and any(c["key"].startswith("GET api.robinhood.com/rhj/assets") for c in upstream.calls()),
              f"catalog after restart: {status}")
        check(KEY.encode() not in body and POLICY.encode() not in body, "a secret reached a response body")
        output += server.stop()
        check(KEY.encode() not in output and POLICY.encode() not in output, "a secret reached process output")
        calls = upstream.calls()
        check(all((KEY in c["url"]) == ("alchemy.com" in c["url"]) for c in calls), "the key only in Alchemy URLs")
    finally:
        upstream.close()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with tempfile.TemporaryDirectory(prefix="robinhood-native-") as root:
        root = Path(root)
        (root / "ca").mkdir()
        ca, _, _, _ = tls_ca.create_certificates(root / "ca", "DNS:localhost")
        production_checks(sys.argv[1], root, ca)
        test_binary_checks(sys.argv[2], root)
    if FAILURES:
        raise SystemExit(f"{len(FAILURES)} failure(s)")
    print("robinhood_native_test: all checks passed")


if __name__ == "__main__":
    main()
