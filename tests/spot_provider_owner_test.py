#!/usr/bin/env python3
"""Local generated-CA integration for the reusable native spot owner."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from collections import Counter
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

import tls_ca


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="spot-provider-owner-") as temp:
        root = Path(temp)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        requests: list[str] = []
        slow_counts: Counter[str] = Counter()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                parsed = urlsplit(self.path)
                values = parse_qs(parsed.query)
                ids = values.get("ids", [""])[0].split(",")
                requests.append(self.path)
                if parsed.path.startswith("/prices/current/"):
                    assert not self.headers.get("x-cg-demo-api-key")
                    assert not self.headers.get("x-cg-pro-api-key")
                    keys = parsed.path.removeprefix("/prices/current/").split(",")
                    if "coingecko:llama-cancel" in keys:
                        (root / "cancel-active.llama").touch()
                        time.sleep(.7)
                    quotes = {key: {"price": 61.25, "timestamp": time.time(), "confidence": 0.99}
                              for key in keys if key in {"coingecko:bitcoin", "coingecko:ethereum", "coingecko:binancecoin",
                              "coingecko:ripple", "coingecko:solana", "coingecko:algorand", "coingecko:llama-cancel",
                              "ethereum:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48"}}
                    if "coingecko:tether" in keys:
                        slow_counts["wiki-cat-fx"] += "coingecko:wiki-cat" in keys
                        fx_age = 3600 if "coingecko:wiki-cat" in keys and slow_counts["wiki-cat-fx"] == 1 else 2
                        quotes["coingecko:tether"] = {"price": .98, "timestamp": time.time() - fx_age, "confidence": .99}
                        quotes["bsc:0x6ec90334d89dbdc89e08a133271be3d104128edb"] = {"price": 100, "timestamp": time.time() - 3600, "confidence": .99}
                    status, body = 200, json.dumps({"coins": quotes}).encode()
                elif parsed.path == "/api/v3/trades":
                    assert values == {"symbol": ["WKCUSDT"], "limit": ["1"]}
                    assert not self.headers.get("x-cg-demo-api-key")
                    assert not self.headers.get("x-cg-pro-api-key")
                    slow_counts["mexc"] += 1
                    age = 3600 if slow_counts["mexc"] == 2 else 0
                    status, body = 200, json.dumps([{"price": "0.04", "time": int((time.time() - age) * 1000)}]).encode()
                elif parsed.path == "/0/public/Trades":
                    assert not self.headers.get("x-cg-demo-api-key")
                    assert not self.headers.get("x-cg-pro-api-key")
                    pair = values.get("pair", [""])[0]
                    assert pair in ("TRXUSD", "AVAXUSD", "TONUSD")
                    assert values.get("count") == ["1"]
                    slow_counts[pair] += 1
                    if pair == "AVAXUSD" and slow_counts[pair] in (2, 3):
                        (root / "cancel-active.kraken").touch()
                        time.sleep(0.7)
                    if pair == "TONUSD" and slow_counts[pair] == 1:
                        status, body = 200, b'{"error":["provider failure"],"result":{}}'
                    else:
                        status, body = 200, json.dumps({"error": [], "result": {
                            pair: [["2.5", "10", time.time(), "b", "m", "", 1]],
                            "last": "1"}}).encode()
                elif any(value in ("tron", "avalanche-2", "the-open-network") for value in ids):
                    slow_counts["native-primary"] += 1
                    if slow_counts["native-primary"] == 1:
                        status, body = 200, b'{"tron":{"usd":0.25}}'
                    else:
                        status, body = 429, b'{"status":{"error_code":10006}}'
                elif any(value in ("bitcoin", "ethereum", "binancecoin", "ripple", "solana", "algorand") for value in ids):
                    status, body = 429, b'{"status":{"error_code":10006}}'
                elif "shared-slow" in ids:
                    (root / "shared-active").touch()
                    time.sleep(0.5)
                    status, body = 200, json.dumps({"shared-slow": {"usd": 4.0}}).encode()
                elif "series-fails" in ids:
                    status, body = 200, b'{"series-fails":{"usd":6.0}}'
                elif "cache-fails" in ids:
                    status, body = 200, b'{"cache-fails":{"usd":6.0}}'
                elif "cancel-all-slow" in ids:
                    slow_counts["cancel-all-slow"] += 1
                    if slow_counts["cancel-all-slow"] == 1:
                        (root / "cancel-all-active").touch()
                        time.sleep(0.5)
                    status, body = 200, json.dumps({"cancel-all-slow": {"usd": 5.0}}).encode()
                elif "shared-failure" in ids:
                    status, body = 400, b'{"error":"shared flight fixture failure"}'
                elif "stale-token" in ids:
                    (root / "stale-active").touch()
                    time.sleep(1.5)
                    status, body = 200, b'{"stale-token":{"usd":42.0}}'
                elif "stale-fail" in ids:
                    (root / "stale-fail-active").touch()
                    status, body = 400, b'{"error":"stale refresh failed"}'
                elif "fg-load-one" in ids or "fg-load-two" in ids:
                    name = "fg-load-one" if "fg-load-one" in ids else "fg-load-two"
                    (root / f"{name}-active").touch()
                    time.sleep(4.0)
                    status, body = 200, b"{}"
                elif "cold-after-stale" in ids:
                    (root / "cold-after-stale-active").touch()
                    status, body = 200, b"{}"
                elif "burst-foreground" in ids:
                    (root / "burst-foreground-active").touch()
                    status, body = 200, b'{"burst-foreground":{"usd":9.0}}'
                elif any(value.startswith("bg") for value in ids):
                    (root / "background-burst-active").touch()
                    time.sleep(0.7)
                    status, body = 200, b"{}"
                elif ids[0] == "cancelme":
                    (root / "cancel-active").touch()
                    time.sleep(0.5)
                    status, body = 200, b"{}"
                elif ids[0].startswith("capacity"):
                    (root / "capacity-active").touch()
                    time.sleep(1.0)
                    status, body = 200, b"{}"
                elif ids[0].endswith("019z"):
                    status, body = 400, b'{"error":"fixture failure"}'
                elif ids[0].endswith("000z"):
                    body = json.dumps({ids[0]: {"usd": 2.5,
                        "usd_24h_change": 1.25, "usd_24h_vol": 400}}).encode()
                    status = 200
                else:
                    status, body = 200, b"{}"
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except OSError:
                    # The cancellation test intentionally closes the first
                    # client while the local handler is delaying its response.
                    pass
                self.close_connection = True

            def log_message(self, *_args) -> None:
                pass

        port = free_port()
        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            marker = root / "cancel-active"
            stop_marker = root / "capacity-active"
            subprocess.run([binary, str(port), str(ca), str(marker), str(stop_marker),
                            str(root / "shared-active"), str(root / "cancel-all-active"),
                            str(root / "stale-active"), str(root / "fg-load-one-active"),
                            str(root / "fg-load-two-active"),
                            str(root / "cold-after-stale-active"),
                            str(root / "stale-fail-active"),
                            str(root / "background-burst-active"),
                            str(root / "burst-foreground-active")],
                           check=True, timeout=60)
            assert len(requests) >= 7, requests
            assert all(path.startswith("/0/public/Trades?pair=") or
                       path.startswith("/prices/current/") or
                       path.startswith("/api/v3/trades?symbol=WKCUSDT&limit=1") or
                       path.startswith("/api/v3/simple/price?ids=") or
                       path.startswith("/assets/price?network_id=0&asset_ids=31566704")
                       for path in requests)
            assert slow_counts["mexc"] == 3 and slow_counts["wiki-cat-fx"] == 3, slow_counts
            assert slow_counts["TRXUSD"] == 1, slow_counts
            assert slow_counts["AVAXUSD"] == 3, slow_counts
            assert slow_counts["TONUSD"] == 2, slow_counts
            assert sum(path.startswith("/assets/price?network_id=0&asset_ids=31566704")
                       for path in requests) == 1, requests
            assert all(len(path.encode()) <= 2048 for path in requests)
            parsed = [parse_qs(urlsplit(path).query).get("ids", [""])[0].split(",")
                      for path in requests]
            assert sum("shared-token" in ids for ids in parsed) == 1, parsed
            assert any("unique-a" in ids and "unique-b" in ids for ids in parsed), parsed
            assert sum("shared-slow" in ids for ids in parsed) == 1, parsed
            assert sum("cancel-all-slow" in ids for ids in parsed) == 2, parsed
            assert sum("shared-failure" in ids for ids in parsed) == 1, parsed
            assert sum("stale-token" in ids for ids in parsed) == 1, parsed
            assert sum("stale-fail" in ids for ids in parsed) == 1, parsed
            assert sum("fg-load-one" in ids for ids in parsed) == 1, parsed
            assert sum("fg-load-two" in ids for ids in parsed) == 1, parsed
            assert sum("cold-after-stale" in ids for ids in parsed) == 1, parsed
            assert all("shed-stale" not in ids for ids in parsed), parsed
            assert any(any(value.startswith("bg") for value in ids) for ids in parsed), parsed
            assert sum("burst-foreground" in ids for ids in parsed) == 1, parsed
            assert all("burst-overflow" not in ids for ids in parsed), parsed
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    run(sys.argv[1])
