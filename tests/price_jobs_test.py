#!/usr/bin/env python3
"""Replay the frozen TypeScript price-jobs contract
(tests/corpus/price_jobs.json) against the native scheduled
price work (src/tokens/price_jobs.cpp) through build/price_jobs_fixture.

Refresh scenarios run through the real coordinator-backed SpotProviderOwner
in background mode against a local HTTPS CoinGecko that answers with the
fixture's reply table (known ids priced, unknown ids omitted, per-fiat
outages). Compared with TypeScript, per scenario:
  - the plan: every spot call's fiat and ids, in order;
  - refreshActive's return value;
  - each demand row's lastRefreshedAt afterwards;
  - the /simple/price requests as a multiset of (fiat, id) over every
    request and attempt, for the prewarm and the run. Native CoinGecko
    batches hold at most 100 ids (kCoinGeckoSpotMaxBatchCount) where
    TypeScript sends up to 250 in one call, so the request count and id
    order inside a request are reported, not compared;
  - restart: no request at boot, then the second run.
The seal scenario must give the same pass counts and candle reads; the
native load checks (1,024 due rows, frames, bytes, reader latency, the
interrupted and SIGKILLed passes) must hold their bounds.
Usage: price_jobs_test.py <price_jobs_fixture binary>
"""
import json
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import tls_ca

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/corpus/price_jobs.json"


def id_multiset(requests):
    return Counter((fiat, i) for fiat, ids in requests for i in ids)


def main(binary):
    fixture = json.loads(FIXTURE.read_text())
    scenarios = {s["name"]: s for s in fixture["scenarios"]}
    # The native-only lane check's reply table.
    upstream = dict(scenarios, lane={"reply": {"prices": {"usd": {"slow-fg": 1.0, "bitcoin": 90000.0}}}})
    failures = []
    with tempfile.TemporaryDirectory(prefix="price-jobs-") as temp:
        root = Path(temp)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        recorded = []  # (scenario, phase, fiat, ids)
        lock = threading.Lock()

        def marker(name):
            try:
                return (root / name).read_text()
            except OSError:
                return ""

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                scenario = upstream.get(marker("scenario"), {})
                phase = marker("phase")
                if not parsed.path.endswith("/simple/price"):
                    body, status = b"{}", 404
                else:
                    fiat = query.get("vs_currencies", [""])[0]
                    ids = [i for i in query.get("ids", [""])[0].split(",") if i]
                    with lock:
                        recorded.append((marker("scenario"), phase, fiat, ids))
                    if "slow-fg" in ids:
                        (root / "slow-started").touch()
                        time.sleep(1.5)
                    reply = scenario.get("reply", {})
                    outage = reply.get("outage", {}).get(fiat)
                    if outage:
                        body, status = b'{"error":"outage"}', outage
                    else:
                        table = reply.get("prices", {}).get(fiat, {})
                        body = json.dumps({i: {fiat: table[i], f"{fiat}_24h_change": 1.5, f"{fiat}_24h_vol": 1000}
                                           for i in ids if i in table}).encode()
                        status = 200
                self.send_response(status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        clock = root / "clock"
        env = dict(os.environ, FORTIS_TEST_PRICE_NOW_FILE=str(clock))
        run = subprocess.run([binary, "replay", str(server.server_address[1]), str(ca), str(FIXTURE), str(root),
                              str(clock)], env=env, capture_output=True, text=True, timeout=600)
        lane = subprocess.run([binary, "lane", str(server.server_address[1]), str(ca), str(root), str(clock)],
                              env=env, capture_output=True, text=True, timeout=120)
        server.shutdown()
        if lane.returncode != 0:
            print(lane.stderr, file=sys.stderr)
            raise SystemExit(f"lane exited {lane.returncode}")
        lane_result = json.loads(lane.stdout)
        if os.environ.get("PRICE_JOBS_TEST_STDERR") == "1":
            print(run.stderr, file=sys.stderr)
        if run.returncode != 0:
            print(run.stderr, file=sys.stderr)
            raise SystemExit(f"replay exited {run.returncode}")
        native = {s["name"]: s for s in json.loads(run.stdout)["scenarios"]}

        def requests_of(name, phase):
            return [(f, ids) for s, p, f, ids in recorded if s == name and p == phase]

        def compare(label, expected, actual):
            if expected != actual:
                failures.append(f"{label}: expected {expected!r}, native {actual!r}")

        for name, ts in scenarios.items():
            if name in ("restart", "seal"):
                continue
            got = native[name]
            plan = [{"fiat": p["fiat"], "ids": p["ids"]} for p in ts["plan"]]
            for p in ts["plan"]:
                compare(f"{name} spot options", {"foreground": False, "background": True}, p["options"])
            compare(f"{name} plan", plan, got["plan"])
            compare(f"{name} refreshed", ts["refreshed"], got["refreshed"])
            compare(f"{name} demand", ts["demand"], got["demand"])
            compare(f"{name} prewarm requests",
                    id_multiset((r["fiat"], r["ids"]) for r in ts["prewarmRequests"]),
                    id_multiset(requests_of(name, "prewarm")))
            compare(f"{name} requests", id_multiset((r["fiat"], r["ids"]) for r in ts["requests"]),
                    id_multiset(requests_of(name, "run")))
            print(f"{name}: plan={len(got['plan'])} calls refreshed={got['refreshed']} "
                  f"requests ts={len(ts['requests'])} native={len(requests_of(name, 'run'))} "
                  f"native={got['native']} {got['elapsedMs']} ms")

        ts = scenarios["restart"]
        got = native["restart"]
        compare("restart boot requests", [], requests_of("restart", "boot"))
        compare("restart requests", id_multiset((r["fiat"], r["ids"]) for ph in ts["phases"] for r in ph["requests"]),
                id_multiset(requests_of("restart", "run")))
        for index, phase in enumerate(ts["phases"]):
            compare(f"restart[{index}] at", phase["at"], got["phases"][index]["at"])
            compare(f"restart[{index}] plan", [{"fiat": p["fiat"], "ids": p["ids"]} for p in phase["plan"]],
                    got["phases"][index]["plan"])
            compare(f"restart[{index}] refreshed", phase["refreshed"], got["phases"][index]["refreshed"])
            compare(f"restart[{index}] demand", phase["demand"], got["phases"][index]["demand"])
        print(f"restart: boot requests={len(requests_of('restart', 'boot'))} "
              f"phases={[p['refreshed'] for p in got['phases']]}")

        # Background mode: shed (no request, no touch) behind a foreground call
        # in flight on the CoinGecko lane; refreshed once the lane is idle.
        compare("lane busy", {"refreshed": 0, "touched": 0},
                {k: lane_result["busy"][k] for k in ("refreshed", "touched")})
        compare("lane busy requests", [("usd", ["slow-fg"])], requests_of("lane", "busy"))
        compare("lane idle", {"refreshed": 1, "touched": 1}, lane_result["idle"])
        compare("lane idle requests", [("usd", ["bitcoin"])], requests_of("lane", "idle"))
        print("lane:", json.dumps(lane_result))

        seal_root = root / "seal"
        seal_root.mkdir()
        sealed = subprocess.run([binary, "seal", str(FIXTURE), str(seal_root)], capture_output=True, text=True,
                                timeout=600)
        if sealed.returncode != 0:
            print(sealed.stderr, file=sys.stderr)
            raise SystemExit(f"seal exited {sealed.returncode}")
        result = json.loads(sealed.stdout)
        ts = scenarios["seal"]
        compare("seal passes", ts["passes"], result["seal"]["passes"])
        compare("seal open pass", ts["openPass"], result["seal"]["openPass"])

        def numeric(reads):
            return [{"interval": r["interval"], "candles": [
                {k: (None if v is None else float(v)) for k, v in c.items()} for c in r["candles"]]}
                for r in reads]
        compare("seal candles before", numeric(ts["before"]), numeric(result["seal"]["before"]))
        compare("seal candles after", numeric(ts["after"]), numeric(result["seal"]["after"]))
        load = result["load"]
        compare("load sealed", 1024, load["sealed"])
        compare("load second pass", 0, load["secondPass"])
        if load["frames"] > 1024 or load["frames"] < 8:
            failures.append(f"load frames out of bounds: {load['frames']}")
        if load["readerDuringSeal"]["reads"] == 0 or load["readerDuringSeal"]["maxUs"] > 250000:
            failures.append(f"reader starved during seal: {load['readerDuringSeal']}")
        interrupted = result["interrupted"]
        compare("interrupted total", 1024, interrupted["firstSealed"] + interrupted["resumeSealed"])
        killed = result["killed"]
        compare("killed total", 1024, killed["sealedBeforeKill"] + killed["resumeSealed"])
        print("seal:", json.dumps(result["seal"]["passes"]), "load:", json.dumps(load))
        print("interrupted:", json.dumps(interrupted), "killed:", json.dumps(killed))

    if failures:
        for failure in failures:
            print("FAIL", failure, file=sys.stderr)
        raise SystemExit(1)
    print("price jobs contract: all scenarios match")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
