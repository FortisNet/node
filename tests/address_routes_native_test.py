#!/usr/bin/env python3
"""Native-only checks of the address routes that the TypeScript
replay (tests/address_routes_test.py) cannot express:

  1. Gating: the normal binary registers none of the eight routes without
     FORTIS_NATIVE_ADDRESS_CANARY=1 (404), all of them with it (an
     unauthenticated request answers 401, not 404), and refuses to start
     with the canary but no CA bundle. GET /v1/dashpay/names/resolve stays
     unregistered in every mode (its proof-verified DPNS client is not
     ported).
  2. Bounds: a provider answer above the 512 KiB native response bound is a
     transport failure (502 UPSTREAM_UNAVAILABLE without a floor), never a
     crash or a truncated body.
  3. Back-pressure and memory: 64 concurrent cold balance reads of distinct
     addresses against a slow upstream answer only 200 or 429 RATE_LIMITED
     (retry-after 2), the process stays up and host RSS growth is reported
     and bounded.
Usage: address_routes_native_test.py <normal binary> <address test binary>
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import address_routes_test as replay
import discovery_route_harness as harness

ROUTES = [
    "/v1/networks/eth-mainnet/address/0x1111111111111111111111111111111111111111/balances",
    "/v1/networks/eth-mainnet/address/0x1111111111111111111111111111111111111111/insight",
    "/v1/networks/eth-mainnet/address/0x1111111111111111111111111111111111111111/nfts",
    "/v1/networks/tron/address/TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf/token-holdings",
    "/v1/networks/eth-mainnet/address/0x1111111111111111111111111111111111111111/transfers",
    "/v1/networks/eth-mainnet/tx/0x" + "ab" * 32,
    "/v1/networks/eth-mainnet/tx/0x" + "ab" * 32 + "/view",
    "/v1/networks/eth-mainnet/overview",
]
DASHPAY = "/v1/dashpay/names/resolve?name=alice"
ETH_RPC = "https://ethereum-rpc.publicnode.com"
NOW = 1767323045000
MAX_RSS_GROWTH_KIB = 256 * 1024


def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else 0


def rpc_body(method, params):
    return json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}, separators=(",", ":"))


class NormalServer:
    def __init__(self, binary, kv_dir, env, expect_start=True):
        self.port = harness.free_port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_")}
        environment.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(self.port)})
        environment.update(env)
        self.process = subprocess.Popen([binary, str(self.port)], env=environment, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        if not expect_start:
            return
        for _ in range(300):
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("server did not start")

    def request(self, *args, **kwargs):
        return harness.Server.request(self, *args, **kwargs)

    def stop(self):
        harness.Server.stop(self)


def check_gating(normal, root, ca, failures):
    for canary, expected in (("0", 404), ("1", 401)):
        kv = Path(root) / f"kv-gating-{canary}"
        kv.mkdir()
        env = {"FORTIS_NATIVE_ADDRESS_CANARY": canary}
        if canary == "1":
            env["FORTIS_CA_BUNDLE_PATH"] = str(ca)
        server = NormalServer(normal, kv, env)
        try:
            for route in ROUTES:
                status, _, body = server.request("GET", route)
                if status != expected:
                    failures.append(f"gating canary={canary} {route}: {status} != {expected} {body[:120]!r}")
            status, _, body = server.request("GET", DASHPAY)
            if status != 404:
                failures.append(f"dashpay canary={canary}: {status} != 404 {body[:120]!r}")
        finally:
            server.stop()
    kv = Path(root) / "kv-no-ca"
    kv.mkdir()
    server = NormalServer(normal, kv, {"FORTIS_NATIVE_ADDRESS_CANARY": "1"}, expect_start=False)
    try:
        code = server.process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server.process.kill()
        code = None
    stderr = server.process.stderr.read().decode()
    if code in (None, 0) or "FORTIS_NATIVE_ADDRESS_CANARY=1 requires FORTIS_CA_BUNDLE_PATH" not in stderr:
        failures.append(f"canary without CA: exit {code} {stderr[-200:]!r}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    normal, binary = sys.argv[1], sys.argv[2]
    failures = []
    addresses = [f"0x{index:040x}" for index in range(1, 65)]
    huge = "0x" + "0" * (600 * 1024)
    providers = {
        f"POST {ETH_RPC} {rpc_body('eth_blockNumber', [])}": {"status": 200, "delayMs": 400,
            "json": {"jsonrpc": "2.0", "id": 1, "result": "0x10"}},
        f"POST {ETH_RPC} {rpc_body('eth_getBalance', ['0x' + 'ee' * 20, '0x10'])}": {"status": 200,
            "json": {"jsonrpc": "2.0", "id": 1, "result": huge}},
        "GET https://api.coingecko.com/api/v3/simple/price?ids=ethereum&vs_currencies=usd&include_24hr_change=true&include_24hr_vol=true":
            {"status": 200, "json": {"ethereum": {"usd": 3000, "usd_24h_change": 1, "usd_24h_vol": 1}}},
    }
    for address in addresses:
        providers[f"POST {ETH_RPC} {rpc_body('eth_getBalance', [address, '0x10'])}"] = {
            "status": 200, "delayMs": 400, "json": {"jsonrpc": "2.0", "id": 1, "result": "0xde0b6b3a7640000"}}
    with tempfile.TemporaryDirectory(prefix="fortis-address-native-") as root:
        table = replay.Table(providers, [])
        upstream = replay.Upstream(root, table)
        try:
            check_gating(normal, root, upstream.ca, failures)
            kv = Path(root) / "kv"
            kv.mkdir()
            clock = Path(root) / "clock"
            harness.set_clock(clock, NOW)
            server = replay.Server(binary, kv, clock, upstream, {})
            token = "fintel_dev_" + "A" * 43

            def get(path):
                return server.request("GET", path, headers={"Authorization": "Bearer " + token,
                                                            "Accept": "application/json"}, timeout=60)
            try:
                # 2. an oversized provider answer is a transport failure.
                status, _, body = get(f"/v1/networks/eth-mainnet/address/0x{'ee' * 20}/balances")
                if status != 502 or json.loads(body) != {"error": "UPSTREAM_UNAVAILABLE"}:
                    failures.append(f"oversized answer: {status} {body[:200]!r}")
                # 3. burst
                before = rss_kib(server.process.pid)
                results = []
                lock = threading.Lock()

                def worker(address):
                    try:
                        status, headers, body = get(f"/v1/networks/eth-mainnet/address/{address}/balances")
                    except OSError as error:
                        status, headers, body = 0, {}, str(error).encode()
                    with lock:
                        results.append((status, headers, body))
                threads = [threading.Thread(target=worker, args=(address,)) for address in addresses]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join(timeout=120)
                after = rss_kib(server.process.pid)
                counts = {}
                for status, headers, body in results:
                    counts[status] = counts.get(status, 0) + 1
                    if status == 429 and (headers.get("retry-after") != "2" or b"RATE_LIMITED" not in body):
                        failures.append(f"burst 429 shape: {headers} {body[:120]!r}")
                    elif status not in (200, 429):
                        failures.append(f"burst status {status}: {body[:160]!r}")
                if server.process.poll() is not None:
                    failures.append("server exited during the burst")
                growth = after - before
                print(f"burst: {counts} rss {before} KiB -> {after} KiB (+{growth} KiB)")
                if counts.get(200, 0) == 0:
                    failures.append("burst served nothing")
                if growth > MAX_RSS_GROWTH_KIB:
                    failures.append(f"rss growth {growth} KiB exceeds {MAX_RSS_GROWTH_KIB} KiB")
            finally:
                server.stop()
        finally:
            upstream.close()
    if failures:
        print(f"address native checks: {len(failures)} failure(s)")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print("address native checks passed")


if __name__ == "__main__":
    main()
