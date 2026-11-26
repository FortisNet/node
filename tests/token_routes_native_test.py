#!/usr/bin/env python3
"""Native-only checks of the token routes that the TypeScript replay
(tests/token_routes_test.py) cannot express:

  1. Gating: the normal binary registers none of the token routes without
     FORTIS_NATIVE_PRICE_ROUTE_CANARY=1 (404), and all of them with it
     (an unauthenticated request answers 401, not 404).
  2. Coverage: every family's address refs are composed. A Robinhood Chain
     stock token is covered since the issuer catalog port
     (tests/token_robinhood_test.py): this upstream answers the
     `/rhj/assets` catalog 404, so its issuer page answers 200 with
     errors.chainMeta, errors.market and errors.profile (in TypeScript's
     order) and no invented identity; a native coin on the same network is
     served. A Solana SPL mint is covered since the
     Jupiter chain-metadata port (tests/token_solana_test.py holds its
     contract): with no Jupiter reply in this upstream its page answers 200
     with errors.chainMeta. A Tron TRC-20 contract is covered since the Tron
     token snapshot port (tests/token_tron_test.py): keyless, its TronGrid
     reads fail here and the driver answers null metadata with no error.
     Stellar and Cardano assets are covered since the asset-index port
     (tests/token_ledger_test.py): this upstream answers their Horizon and
     Koios reads 404, so the page answers 200 with errors.holders and
     errors.insight and no invented count. Algorand ASAs and XRPL issued
     tokens are covered since the issuer-insight port
     (tests/token_issuer_test.py): this upstream answers their algod GET and
     XRPL JSON-RPC POSTs 404 (the POSTs fail over across all three XRPL
     endpoints), so the page answers 200 with errors.insight, while holders
     stay null with no error (neither driver has a holder count). Hedera
     HTS tokens are covered since the Mirror Node and SaucerSwap ports
     (tests/token_hedera_test.py, tests/token_saucerswap_test.py): this
     upstream answers both Mirror GETs 404, so the default page answers 200
     with errors.chainMeta and errors.holders, and the SaucerSwap token,
     prices and pools GETs 404 too, SaucerSwapClient's durable "unlisted"
     null: market, profile and liquidity are null with no error. Sui
     Coin<T> types are covered since the GraphQL coinMetadata port
     (tests/token_sui_test.py): this upstream answers the GraphQL POST 404
     (an answer, not an outage: no negative chainMeta is cached), so the
     page answers 200 with errors.chainMeta only; holders stay null with no
     error and insight is [] (the Sui driver has neither read). Bittensor
     subnet tokens are covered since the Taostats port
     (tests/token_bittensor_test.py): this server has no TAOSTATS_API_KEY,
     so there is no Bittensor driver and the page answers honest nulls
     (identity without a symbol, market null, insight []) with no error and
     no Taostats request.
     They are checked last so their CoinGecko resolution does not spend the
     burst's lane budget.
  3. The `series` section over this fixture's upstream (no market_chart
     reply): a page with a price id swallows the failed backfill like
     CandleHistory and answers an empty series with cache.hit true and no
     error, never an invented one; without a price id it is null with no
     error. The full series contract is tests/token_series_test.py.
  4. Back-pressure and memory: 48 concurrent cold page reads against a slow
     upstream answer only 200 or 429 RATE_LIMITED (retry-after 2), the
     process stays up, and host RSS growth stays bounded.
Usage: token_routes_native_test.py <normal binary> <token test binary>
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import discovery_route_harness as harness
import token_routes_test as replay

# A Robinhood Chain token: covered by the issuer catalog port.
ROBINHOOD_TOKEN = "0x1fbd4a0f0ea9e2a1a5a7b8c9d0e1f2a3b4c5d6e7"
# A Bittensor subnet token: covered by the keyed Taostats port; unkeyed here.
BITTENSOR_SUBNET = "bittensor-alpha:1"
# A Sui coin type: covered by the GraphQL coinMetadata port.
SUI_COIN = "0xdba34672e30cb065b1f93e3ab55318768fd6fef66c15942c9f7cb846e2f900e7::usdc::USDC"

ROUTES = [
    "/v1/networks/eth-mainnet/token/native",
    "/v1/networks/eth-mainnet/token/native/page",
    "/v1/networks/eth-mainnet/token/native/holders/history",
    "/v1/networks/eth-mainnet/token/native/liquidity/history",
    "/v1/networks/eth-mainnet/token/native/liquidity/pools",
    "/v1/networks/eth-mainnet/token/native/liquidity/summary",
]


def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else 0


class NormalServer:
    def __init__(self, binary, kv_dir, env):
        self.port = harness.free_port()
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("FORTIS_") and k not in ("COINGECKO_API_KEY",)}
        environment.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(self.port)})
        environment.update(env)
        self.process = subprocess.Popen([binary, str(self.port)], env=environment, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        for _ in range(300):
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + self.process.stderr.read().decode())
            try:
                import socket
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
        env = {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": canary}
        if canary == "1":
            env["FORTIS_CA_BUNDLE_PATH"] = str(ca)
        server = NormalServer(normal, kv, env)
        try:
            for route in ROUTES:
                status, _, body = server.request("GET", route)
                if status != expected:
                    failures.append(f"gating canary={canary} {route}: {status} != {expected} {body[:120]!r}")
            status, _, body = server.request("GET", "/v1/gems")
            if status != 404:
                failures.append(f"removed gems route canary={canary}: {status} != 404 {body[:120]!r}")
        finally:
            server.stop()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    normal, binary = sys.argv[1], sys.argv[2]
    fixture = harness.load_fixture(replay.FIXTURE)
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-token-native-") as root:
        rules = replay.Rules(fixture["providers"])
        upstream = replay.Upstream(root, rules)
        check_gating(normal, root, upstream.ca, failures)
        kv = Path(root) / "kv"
        kv.mkdir()
        clock = Path(root) / "clock"
        now = replay.NOW
        harness.set_clock(clock, now)
        server = replay.Server(binary, kv, clock, upstream, now)
        tokens = harness.TokenRotation(per_token=30)

        def get(path, accept="application/json"):
            return server.request("GET", path, headers={"Authorization": "Bearer " + tokens.next(),
                                                        "Accept": accept})
        try:
            # 2. coverage
            before = len(upstream.calls())
            status, _, body = get(f"/v1/networks/robinhood/token/{ROBINHOOD_TOKEN}/page"
                                  "?include=identity,market,profile")
            page = json.loads(body) if status == 200 else {}
            identity = page.get("identity") or {}
            catalog = [c for c in upstream.calls()[before:] if c[1] == "api.robinhood.com"]
            if status != 200 or list((page.get("errors") or {}).items()) != [
                    ("chainMeta", "UPSTREAM_UNAVAILABLE"), ("market", "UPSTREAM_UNAVAILABLE"),
                    ("profile", "UPSTREAM_UNAVAILABLE")] or identity.get("symbol") is not None or \
                    identity.get("verified") is not None or page.get("market") is not None or \
                    page.get("profile") is not None or [c[2] for c in catalog] != ["/rhj/assets"]:
                failures.append(f"covered Robinhood token page: {status} {body[:300]!r} {catalog}")
            status, _, body = get("/v1/networks/robinhood/token/native/page?include=identity")
            if status != 200 or json.loads(body)["identity"]["symbol"] != "ETH":
                failures.append(f"covered native Robinhood page: {status} {body[:200]!r}")
            status, _, body = get(f"/v1/networks/robinhood/token/{ROBINHOOD_TOKEN}/holders/history")
            if status != 200:
                failures.append(f"store-only Robinhood holder history: {status}")
            # 4. back-pressure and memory under a burst of distinct cold pages
            original = upstream.rules

            def slow(method, host, target, payload):
                time.sleep(.4)
                return original(method, host, target, payload)

            upstream.rules = slow
            harness.set_clock(clock, now + 40_000)
            before = rss_kib(server.process.pid)
            statuses = []
            lock = threading.Lock()

            def one(i):
                address = "0x" + f"{i + 1:040x}"
                path = f"/v1/networks/base/token/{address}/page?include=identity,liquidity,contract"
                with lock:
                    token = tokens.next()
                try:
                    status, headers, _ = server.request("GET", path, headers={"Authorization": "Bearer " + token},
                                                        timeout=60)
                except OSError as error:
                    status, headers = f"error {error}", {}
                with lock:
                    statuses.append((status, headers.get("retry-after")))

            threads = [threading.Thread(target=one, args=(i,)) for i in range(48)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=90)
            upstream.rules = original
            after = rss_kib(server.process.pid)
            unexpected = [s for s in statuses if not (s[0] == 200 or (s[0] == 429 and s[1] == "2"))]
            if len(statuses) != 48 or unexpected:
                failures.append(f"burst statuses: {len(statuses)} answered, unexpected {unexpected[:5]}")
            if server.process.poll() is not None:
                failures.append("server exited during the burst")
            accepted = sum(1 for s in statuses if s[0] == 200)
            limited = sum(1 for s in statuses if s[0] == 429)
            growth = after - before
            print(f"burst: {accepted} served, {limited} back-pressured (429); RSS {before} -> {after} KiB "
                  f"(+{growth} KiB)")
            if growth > 96 * 1024:
                failures.append(f"RSS grew {growth} KiB under the burst")
            # 3. series, after the burst: its backfill attempt spends a CoinGecko
            # lane token, and the burst is paced by that lane (16 contract lookups).
            harness.set_clock(clock, now + 60_000)
            status, _, body = get("/v1/networks/bitcoin/token/native/page?include=identity,series")
            doc = json.loads(body) if status == 200 else {}
            empty = {"interval": "1d", "points": [], "volumes": [], "window": {"from": None, "to": None, "days": 0}}
            if status != 200 or doc.get("series") != empty or doc.get("errors") != {} \
                    or doc.get("cache", {}).get("series") != {"hit": True, "timestamp": None}:
                failures.append(f"series with a price id: {status} {body[:300]!r}")
            status, _, body = get("/v1/networks/eth-mainnet/token/symbol%3Aunlisted/page?include=identity,series")
            doc = json.loads(body) if status == 200 else {}
            if status != 200 or doc.get("series") is not None or "series" in doc.get("errors", {"series": 1}):
                failures.append(f"series without a price id: {status} {body[:300]!r}")
            # 2 (continued). A Solana SPL mint is served; this upstream has no
            # Jupiter reply, so its chain metadata is unavailable.
            harness.set_clock(clock, now + 90_000)
            status, _, body = get("/v1/networks/solana/token/EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
                                  "/page?include=identity,insight")
            doc = json.loads(body) if status == 200 else {}
            if status != 200 or doc.get("identity", {}).get("kind") != "spl" or \
                    doc.get("errors", {}).get("chainMeta") != "UPSTREAM_UNAVAILABLE" or doc.get("insight") != []:
                failures.append(f"covered SPL page: {status} {body[:300]!r}")
            # 2 (continued). A Tron TRC-20 contract is served; keyless, its
            # TronGrid reads fail against this upstream and the driver's
            # snapshot is null: no metadata, no insight, no error.
            harness.set_clock(clock, now + 100_000)
            status, _, body = get("/v1/networks/tron/token/TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t"
                                  "/page?include=identity,insight")
            doc = json.loads(body) if status == 200 else {}
            if status != 200 or doc.get("identity", {}).get("kind") != "trc20" or doc.get("insight") != [] or \
                    "chainMeta" in doc.get("errors", {"chainMeta": 1}):
                failures.append(f"covered TRC-20 page: {status} {body[:300]!r}")
            # 2 (continued). Stellar and Cardano assets are served; their
            # index reads answer 404 here, which the drivers throw.
            for network, ref in (
                    ("stellar", "USDC-GA5ZSEJYB37JRC5AVCIA5MOP4RHTM335X2KGX3IHOJAPP5RE34K4KZVN"),
                    ("cardano", "a0028f350aaabe0545fdcb56b039bfb08e4bb4d8c4d7c3c7d481c235484f534b59")):
                harness.set_clock(clock, now + 110_000)
                status, _, body = get(f"/v1/networks/{network}/token/{ref}/page?include=identity,holders,insight")
                doc = json.loads(body) if status == 200 else {}
                errors = doc.get("errors", {})
                if status != 200 or doc.get("identity", {}).get("kind") != network or doc.get("holders") is not None \
                        or errors.get("holders") != "UPSTREAM_UNAVAILABLE" or \
                        errors.get("insight") != "UPSTREAM_UNAVAILABLE" or doc.get("insight") != []:
                    failures.append(f"covered {network} asset page: {status} {body[:300]!r}")
            # 2 (continued). Algorand ASAs and XRPL issued tokens are served;
            # their algod GET and JSON-RPC POSTs answer 404 here, which the
            # drivers throw. Holders are null with no error.
            for network, ref, kind in (
                    ("algorand", "31566704", "asa"),
                    ("xrpl", "USD.rvYAfWj5gh67oV6fW32ZzP3Aw4Eubs59B", "xrpl")):
                harness.set_clock(clock, now + 120_000)
                status, _, body = get(f"/v1/networks/{network}/token/{ref}/page?include=identity,holders,insight")
                doc = json.loads(body) if status == 200 else {}
                errors = doc.get("errors", {})
                if status != 200 or doc.get("identity", {}).get("kind") != kind or doc.get("holders") is not None \
                        or "holders" in errors or errors.get("insight") != "UPSTREAM_UNAVAILABLE" or \
                        doc.get("insight") != []:
                    failures.append(f"covered {network} token page: {status} {body[:300]!r}")
            # 2 (continued). Hedera HTS tokens are served; both Mirror GETs
            # answer 404 here, which the driver throws, and the SaucerSwap
            # GETs 404, an unlisted token (null sections, no error).
            harness.set_clock(clock, now + 120_000)
            status, _, body = get("/v1/networks/hedera/token/0.0.456858/page")
            doc = json.loads(body) if status == 200 else {}
            errors = doc.get("errors", {})
            if status != 200 or doc.get("identity", {}).get("kind") != "hts" or doc.get("holders") is not None or \
                    errors != {"chainMeta": "UPSTREAM_UNAVAILABLE", "holders": "UPSTREAM_UNAVAILABLE"} or \
                    doc.get("market") is not None or doc.get("liquidity") is not None:
                failures.append(f"covered hedera token page: {status} {body[:300]!r}")
            status, _, body = get("/v1/networks/hedera/token/0.0.456858/holders/history")
            if status != 200:
                failures.append(f"hedera holder history: {status}")
            # 2 (continued). Sui coin types are served; the GraphQL POST
            # answers 404 here, which the driver throws (a failure, never a
            # negative answer).
            for include in ("", "?include=identity,holders,insight"):
                status, _, body = get(f"/v1/networks/sui/token/{SUI_COIN}/page{include}")
                doc = json.loads(body) if status == 200 else {}
                identity = doc.get("identity", {})
                if status != 200 or identity.get("kind") != "sui" or identity.get("decimals") is not None or \
                        doc.get("errors") != {"chainMeta": "UPSTREAM_UNAVAILABLE"} or \
                        doc.get("holders") is not None or (include and doc.get("insight") != []) or \
                        b"graphql.mainnet.sui.io" in body:
                    failures.append(f"covered sui token page{include}: {status} {body[:300]!r}")
            status, _, body = get(f"/v1/networks/sui/token/{SUI_COIN}/holders/history")
            if status != 200:
                failures.append(f"sui holder history: {status}")
            # 2 (continued). Bittensor subnets are served; without a Taostats
            # key there is no driver: honest nulls, no Taostats request.
            before = len(upstream.calls())
            status, _, body = get(f"/v1/networks/bittensor/token/{BITTENSOR_SUBNET}/page"
                                  "?include=identity,market,insight")
            doc = json.loads(body) if status == 200 else {}
            identity = doc.get("identity", {})
            taostats = [c for c in upstream.calls()[before:] if "taostats" in c[1]]
            if status != 200 or identity.get("kind") != "bittensor-alpha" or identity.get("symbol") is not None or \
                    doc.get("market") is not None or doc.get("insight") != [] or doc.get("errors") != {} or taostats:
                failures.append(f"covered unkeyed bittensor subnet page: {status} {body[:300]!r} {taostats}")
            status, _, body = get("/v1/networks/bittensor/token/native/page?include=identity,market")
            if status != 200 or json.loads(body)["identity"]["symbol"] != "TAO":
                failures.append(f"covered native TAO page: {status} {body[:200]!r}")
            status, _, body = get(f"/v1/networks/bittensor/token/{BITTENSOR_SUBNET}/holders/history")
            if status != 200:
                failures.append(f"bittensor holder history: {status}")
        finally:
            server.stop()
            upstream.close()
    if failures:
        print(f"token routes native checks: {len(failures)} failure(s)")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print("token routes native checks passed")


if __name__ == "__main__":
    main()
