#!/usr/bin/env python3
"""POST /v1/prices/markets feeds the coin popularity tracker as
TokenDiscoveryFacet.markets does (ids trimmed, Unicode lower-cased,
de-duplicated and sorted before `popularity.record(ids, deviceId)`), through
build/fortis_node_discovery_route_test with no reachable upstream (every
cold coin answers `pending`; recording happens before any provider work).

The frozen TypeScript `recording` scenario's market calls are replayed with
two devices; the tracker rows of the coins only those calls touch (alpha,
beta, Ünï: ids are ASCII lower-cased) must equal the
TypeScript states bit for bit. A request that fails validation records
nothing.
Usage: coin_popularity_markets_test.py <fortis_node_discovery_route_test>
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/corpus/coin_popularity.json"
TOKENS = {"device-a": "fintel_dev_" + "A" * 43, "device-b": "fintel_dev_" + "B" * 43,
          None: "fintel_dev_" + "C" * 43}
NOW = 1767323045000


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def request(port, method, path, body=None, token=None):
    payload = json.dumps(body).encode() if body is not None else b""
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close", "Accept: application/json"]
    if token:
        lines.append(f"Authorization: Bearer {token}")
    if body is not None:
        lines += ["Content-Type: application/json", f"Content-Length: {len(payload)}"]
    with socket.create_connection(("127.0.0.1", port), timeout=60) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + payload)
        data = bytearray()
        while chunk := sock.recv(65536):
            data.extend(chunk)
    head, _, rest = bytes(data).partition(b"\r\n\r\n")
    return int(head.split()[1]), rest


def main(binary):
    recording = json.loads(FIXTURE.read_text())["recording"]
    failures = []

    def check(label, condition, detail=""):
        print(("ok   " if condition else "FAIL ") + label + (f": {detail}" if detail else ""))
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory(prefix="coin-popularity-markets-") as temp:
        root = Path(temp)
        kv = root / "kv"
        kv.mkdir()
        clock = root / "clock"
        port = free_port()
        env = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_")}
        env.update({"FORTIS_ENABLE_TEST_DISCOVERY_ROUTES": "1", "FORTIS_TEST_DISCOVERY_NOW_FILE": str(clock),
                    "FORTIS_KV_DIR": str(kv), "PORT": str(port)})
        clock.write_text(f"{NOW}\n")
        process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            for _ in range(300):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.03)
            status, _ = request(port, "POST", "/v1/prices/markets", {"ids": ["alpha"], "fiat": "eur"},
                                TOKENS["device-a"])
            status_probe, body = request(port, "GET", "/probe/test-coin-popularity")
            check("an invalid body records nothing", status == 400 and json.loads(body)["coins"] == [],
                  f"{status} {status_probe}")
            # The TypeScript scenario's market calls run at steps 5, 6 and 7
            # (15 s apart, after the five price calls).
            for offset, call in enumerate(recording["markets"]):
                clock.write_text(f"{NOW + 15000 * (5 + offset)}\n")
                status, body = request(port, "POST", "/v1/prices/markets", {"ids": call["ids"]},
                                       TOKENS[call.get("deviceId")])
                check(f"POST /v1/prices/markets step {5 + offset}", status == 200, body[:120])
            status, body = request(port, "GET", "/probe/test-coin-popularity")
            native = {c["id"]: c for c in json.loads(body)["coins"]}
            expected = {c["id"]: c for c in recording["afterMarkets"]}
            # js-case: ids are ASCII lower-cased, so "Ünï" keeps its
            # "Ü" (TypeScript recorded the Unicode-lowered "ünï").
            expected["Ünï"] = dict(expected.pop("ünï"), id="Ünï")
            for coin in ("beta", "Ünï"):
                check(f"markets recorded {coin} as TypeScript", native.get(coin) == expected[coin],
                      f"native {native.get(coin)} ts {expected[coin]}")
            # The scenario's last call is device-less (a direct facet call);
            # the route always authenticates a device (token C here), which
            # adds exactly one user at that instant and the same hit.
            alpha = dict(expected["alpha"], users=expected["alpha"]["users"] + 1)
            check("markets recorded alpha as TypeScript (+1 user for the authenticated device)",
                  native.get("alpha") == alpha, f"native {native.get('alpha')} ts+1 {alpha}")
            check("markets bitcoin: one hit from device C", native.get("bitcoin", {}).get("users") == 1 and
                  native["bitcoin"]["hits"] == 1, json.dumps(native.get("bitcoin")))
        finally:
            process.terminate()
            process.wait(timeout=30)
    if failures:
        raise SystemExit(f"{len(failures)} failure(s)")
    print("coin popularity markets recording: all checks pass")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
