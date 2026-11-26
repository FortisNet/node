#!/usr/bin/env python3
"""Native-only behaviour of the cross-chain routes that the frozen
TypeScript replay (cross_chain_route_test.py) does not reach.

Production composition (build/fortis_node, development enrollment):
  - FORTIS_NATIVE_CROSS_CHAIN_CANARY unset/0 leaves the three routes absent
    (404 where the canary answers 401); an invalid canary value, a missing
    CA bundle, MAYA_DASH_EXECUTION_ENABLED other than true/false, execution
    without two distinct https MAYA_NODE_URLS, a non-https MAYA_NODE_URLS
    entry and an over-long NEAR_INTENTS_JWT stop startup;
  - with no provider credential a full quote reaches no upstream (every
    attempt not-configured/no-route) and the provider directory reports the
    configuration (MAYA quote/execution only with execution on two roots).

Test composition (build/fortis_node_cross_chain_route_test, one
loopback upstream for every provider host):
  - the 25 s provider deadline (shortened by FORTIS_TEST_CROSS_CHAIN_DEADLINE_MS):
    a stalled LI.FI answer becomes a `failed` attempt next to Squid's route
    and a stalled status a 502, both inside the deadline;
  - saturation: 30 concurrent quotes against a slow upstream are all
    answered, either 200 or the typed 429 back-pressure, never 5xx or a
    timeout, and the next quote succeeds once the queue drains;
  - a SIGKILL + restart serves the routes again (nothing is durable);
  - provider credentials reach upstream headers only: never a response body
    or process output.
Usage: cross_chain_native_test.py <fortis_node> <fortis_node_cross_chain_route_test>
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
from cross_chain_route_test import Server, Upstream, free_port

FAILURES = []
SECRETS = {
    "LIFI_API_KEY": "native-lifi-secret-123456",
    "SQUID_INTEGRATOR_ID": "native-squid-integrator-123456",
    "SIMPLESWAP_API_KEY": "native-simpleswap-secret-123456",
    "NEAR_INTENTS_JWT": "native-near-jwt-123456",
}
NATIVE = "0x0000000000000000000000000000000000000000"
QUOTE = {"from": {"chainId": "1", "token": NATIVE, "decimals": 18, "address": "0x" + "11" * 20},
         "to": {"chainId": "137", "token": "0x" + "22" * 20, "decimals": 6, "address": "0x" + "33" * 20},
         "amountIn": "1000000000000000000", "slippageBps": 50}


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


class Production:
    def __init__(self, binary, root, env):
        self.port = free_port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_") and k not in SECRETS
                       and not k.startswith("MAYA_")}
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


def production_checks(binary, root, ca):
    # Absent without the canary.
    for value in (None, "0", ""):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_CROSS_CHAIN_CANARY"] = value
        process = Production(binary, root / f"off-{value}", env)
        check(process.started, f"canary {value!r}: process starts")
        if process.started:
            for method, path in (("GET", "/v1/exchange/cross-chain/providers"),
                                 ("POST", "/v1/exchange/cross-chain/quotes"),
                                 ("POST", "/v1/exchange/cross-chain/status")):
                status, _, _ = http(process.port, method, path, body={} if method == "POST" else None)
                check(status == 404, f"canary {value!r}: {path} answers {status}, not 404")
        process.stop()
    # Startup refusals.
    refusals = [
        ({"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "2", "FORTIS_CA_BUNDLE_PATH": str(ca)}, "must be 0 or 1"),
        ({"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1"}, "requires FORTIS_CA_BUNDLE_PATH"),
        ({"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
          "MAYA_DASH_EXECUTION_ENABLED": "yes"}, "MAYA_DASH_EXECUTION_ENABLED must be true or false"),
        ({"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
          "MAYA_DASH_EXECUTION_ENABLED": "true", "MAYA_NODE_URLS": "https://a.example/x,https://A.example:443/y"},
         "requires at least two independent HTTPS MAYA_NODE_URLS"),
        ({"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
          "MAYA_NODE_URLS": "http://a.example"}, "MAYA_NODE_URLS must list https:// URLs"),
        ({"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
          "NEAR_INTENTS_JWT": "j" * 249}, "NEAR_INTENTS_JWT is too long"),
    ]
    for index, (env, message) in enumerate(refusals):
        process = Production(binary, root / f"refuse-{index}", env)
        output = process.stop()
        check(not process.started and message in output, f"refusal {index}: {message!r} not in {output[-200:]!r}")
    # On, with no provider credential: no upstream traffic for a full quote.
    process = Production(binary, root / "on", {"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca)})
    check(process.started, "canary on: process starts")
    if process.started:
        status, _, _ = http(process.port, "GET", "/v1/exchange/cross-chain/providers")
        check(status == 401, f"canary on: providers without a bearer answers {status}")
        token = enroll(process.port)
        status, _, body = http(process.port, "GET", "/v1/exchange/cross-chain/providers", token)
        providers = {p["id"]: p for p in json.loads(body)["providers"]}
        check(status == 200 and not providers["lifi"]["configured"] and providers["near-intents"]["configured"]
              and not providers["maya"]["capabilities"]["quote"], f"directory without credentials: {body[:200]!r}")
        started = time.monotonic()
        status, _, body = http(process.port, "POST", "/v1/exchange/cross-chain/quotes", token, QUOTE)
        attempts = [(a["providerId"], a["status"]) for a in json.loads(body)["attempts"]]
        check(status == 200 and attempts == [("simpleswap", "not-configured"), ("maya", "no-route"),
                                             ("bridge-vet", "no-route"), ("lifi", "not-configured"),
                                             ("squid", "not-configured"), ("near-intents", "no-route")],
              f"unconfigured quote: {status} {attempts}")
        check(time.monotonic() - started < 2, "unconfigured quote reached no upstream (answered at once)")
    process.stop()
    process = Production(binary, root / "maya", {"FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
                                                 "MAYA_DASH_EXECUTION_ENABLED": "true",
                                                 "MAYA_NODE_URLS": "https://a.example/x/,https://b.example"})
    check(process.started, "maya execution: process starts")
    if process.started:
        token = enroll(process.port)
        _, _, body = http(process.port, "GET", "/v1/exchange/cross-chain/providers", token)
        maya = {p["id"]: p for p in json.loads(body)["providers"]}["maya"]
        check(maya["capabilities"] == {"quote": True, "execution": True, "status": True}, f"maya enabled: {maya}")
    process.stop()


def lifi_quote_reply():
    return {"id": "q-1", "tool": "gasZip", "toolDetails": {"key": "gasZip", "name": "GasZip"},
            "estimate": {"toAmount": "1000", "toAmountMin": "900"},
            "transactionRequest": {"to": "0x" + "77" * 20, "data": "0x12"}}


def squid_reply():
    return {"quoteId": "s-1", "route": {"estimate": {"toAmount": "950", "toAmountMin": "940", "actions": []},
                                         "transactionRequest": {"target": "0x" + "88" * 20, "data": "0xab"}}}


def table(lifi_delay=0.0, squid_delay=0.0, status_delay=0.0):
    return {
        "GET li.quest/v1/quote": [{"status": 200, "body": json.dumps(lifi_quote_reply()), "delay": lifi_delay}],
        "POST v2.api.squidrouter.com/v2/route": [{"status": 200, "body": json.dumps(squid_reply()), "delay": squid_delay}],
        "GET li.quest/v1/status": [{"status": 200, "body": json.dumps({"status": "DONE"}), "delay": status_delay}],
    }


class SlowUpstream(Upstream):
    """The replay upstream with an optional per-reply delay."""

    def __init__(self, root):
        super().__init__(root)
        handler = self.server.RequestHandlerClass
        original = handler.handle_any
        upstream = self

        def delayed(self_handler):
            key = None
            with upstream.lock:
                for k, entries in upstream.table.items():
                    method, rest = k.split(" ", 1)
                    host = self_handler.headers.get("x-fortis-test-host", "")
                    if method == self_handler.command and rest == host + self_handler.path.split("?")[0]:
                        key = k
            delay = upstream.table.get(key, [{}])[0].get("delay", 0) if key else 0
            if delay:
                time.sleep(delay)
            return original(self_handler)

        handler.do_GET = delayed
        handler.do_POST = delayed


def test_binary_checks(binary, root):
    (root / "tls").mkdir()
    upstream = SlowUpstream(root / "tls")
    clock = root / "clock"
    clock.write_text("0")
    token = "fintel_dev_" + "A" * 43
    try:
        # Deadline: a stalled LI.FI quote is a failed attempt next to Squid.
        (root / "kv-deadline").mkdir()
        server = Server(binary, upstream, root / "kv-deadline", clock,
                        dict(SECRETS, FORTIS_TEST_CROSS_CHAIN_DEADLINE_MS="1500"))
        upstream.start_case(table(lifi_delay=4))
        started = time.monotonic()
        status, _, body = http(server.port, "POST", "/v1/exchange/cross-chain/quotes", token, QUOTE)
        elapsed = time.monotonic() - started
        attempts = {a["providerId"]: a["status"] for a in json.loads(body)["attempts"]}
        check(status == 200 and attempts.get("lifi") == "failed" and attempts.get("squid") == "quoted",
              f"deadline quote: {status} {attempts}")
        check(elapsed < 3.5, f"deadline quote answered after {elapsed:.1f}s")
        upstream.start_case(table(status_delay=4))
        started = time.monotonic()
        status, _, body = http(server.port, "POST", "/v1/exchange/cross-chain/status", token,
                               {"providerId": "lifi", "txHash": "0x" + "ab" * 32, "fromChainId": "1", "toChainId": "137"})
        check(status == 502 and time.monotonic() - started < 3.5, f"deadline status: {status} {body[:80]!r}")
        output = server.stop()
        for secret in SECRETS.values():
            check(secret.encode() not in output, "a credential reached process output")
        # Saturation: every request is answered, 200 or the typed 429.
        time.sleep(4)
        (root / "kv-load").mkdir()
        server = Server(binary, upstream, root / "kv-load", clock, dict(SECRETS))
        upstream.start_case(table(lifi_delay=1.5, squid_delay=1.5))
        results = []
        lock = threading.Lock()

        def one(index):
            letter = "ABCD"[index % 4]
            try:
                status, headers, body = http(server.port, "POST", "/v1/exchange/cross-chain/quotes",
                                             "fintel_dev_" + letter * 43, QUOTE, timeout=45)
            except OSError as error:
                status, headers, body = -1, {}, str(error).encode()
            with lock:
                results.append((status, headers.get("retry-after"), body))

        threads = [threading.Thread(target=one, args=(i,)) for i in range(30)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        statuses = sorted(r[0] for r in results)
        check(all(s in (200, 429) for s in statuses), f"saturation statuses: {statuses}")
        check(statuses.count(200) >= 1, f"saturation: some quotes succeed ({statuses})")
        for status, retry, body in results:
            if status == 429:
                check(retry is not None and json.loads(body)["error"] == "RATE_LIMITED", f"429 shape: {retry} {body[:80]!r}")
            if status == 200:
                check(SECRETS["LIFI_API_KEY"].encode() not in body, "a credential reached a response body")
        print(f"saturation: {statuses.count(200)} x 200, {statuses.count(429)} x 429")
        calls = upstream.calls()
        lifi_headers = [c["headers"].get("x-lifi-api-key") for c in calls if "li.quest" in c["url"]]
        check(lifi_headers and all(h == SECRETS["LIFI_API_KEY"] for h in lifi_headers), "LI.FI key header on every call")
        # Restart after SIGKILL: the routes serve again.
        server.process.send_signal(signal.SIGKILL)
        server.process.wait()
        server = Server(binary, upstream, root / "kv-load", clock, dict(SECRETS))
        upstream.start_case(table())
        status, _, body = http(server.port, "POST", "/v1/exchange/cross-chain/quotes", token, QUOTE)
        check(status == 200 and len(json.loads(body)["routes"]) == 2, f"after restart: {status} {body[:120]!r}")
        server.stop()
    finally:
        upstream.close()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with tempfile.TemporaryDirectory(prefix="cross-chain-native-") as root:
        root = Path(root)
        (root / "ca").mkdir()
        ca, _, _, _ = tls_ca.create_certificates(root / "ca", "DNS:localhost")
        production_checks(sys.argv[1], root, ca)
        test_binary_checks(sys.argv[2], root)
    if FAILURES:
        raise SystemExit(f"{len(FAILURES)} failure(s)")
    print("cross_chain_native_test: all checks passed")


if __name__ == "__main__":
    main()
