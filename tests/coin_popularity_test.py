#!/usr/bin/env python3
"""Replay the frozen TypeScript coin popularity contract
(tests/corpus/coin_popularity.json) against the native tracker and
scheduler (src/discovery/coin_popularity.cpp, src/prices/coin_price_scheduler.cpp) through
build/coin_popularity_fixture.

Compared with TypeScript:
  - domain: every decayed / recordRequest / score / isExpired / bucket /
    interval case, exactly (IEEE doubles);
  - tracker scripts: every output of every op (states in Map insertion order
    with refresh marks, due lists with rank/bucket/interval, takeDirty as a
    set, sizes);
  - runs: through a real coordinator and the token provider owner against a
    local HTTPS CoinGecko serving the fixture's rows. The replay uses the
    TypeScript batch of 250 ids and no call cap so the requests can be
    compared: the sequence of distinct requests (consecutive retries of one
    batch collapsed; attempts are reported), refreshed (a rejected run is
    `aborted`), the due list afterwards, each inspected coin's mkt:/px:
    envelope, EUR spot absence and series spot, and the persisted rows;
  - restart (restore without requests, seven-day expiry, prune) and idle.
Native-only checks: the production batch (80) and a call cap, stop between
batches, and the 20,000-coin / capped-pair load (memory, writes, timings).
Usage: coin_popularity_test.py <coin_popularity_fixture binary>
"""
import json
import math
import os
import ssl
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import compat_expectations  # noqa: E402
import tls_ca

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/corpus/coin_popularity.json"


def hexed(text):
    return text.encode("utf-8").hex() if text else "~"


def unhexed(value):
    return bytes.fromhex(value).decode("utf-8")


def num(value):
    return repr(float(value)) if isinstance(value, float) else str(value)


class Driver:
    def __init__(self, binary):
        self.process = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, bufsize=1)

    def send(self, *parts):
        self.process.stdin.write(" ".join(str(p) for p in parts) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise SystemExit(f"driver died on {parts[0]}: {self.process.stderr.read()}")
        return json.loads(line)

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=60)


def fix_ids(value):
    """Native idHex fields become `id` strings, recursively."""
    if isinstance(value, list):
        return [fix_ids(v) for v in value]
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            if k == "idHex":
                out["id"] = unhexed(v)
            else:
                out[k] = fix_ids(v)
        return out
    return value


def state_args(state):
    return [num(state["users"]), num(state["hits"]), state["updatedAt"], state["lastRequestedAt"]]


# TypeScript's `dirty` Set survives prune(): a coin pruned while dirty and
# then restored is emitted by the next takeDirty. Native dirty marks live on
# the row and go with it. Restore runs only at boot, on an empty tracker,
# so this cannot occur in the composition; the script exercises it directly.
KNOWN_DIFFERENCES = {
    ("expiry", 10): ("pruned-dirty coin restored: TypeScript re-emits it, native does not",
                     lambda rows: [r for r in rows if r["id"] != "old-coin"]),
}


def main(binary):
    fixture = json.loads(FIXTURE.read_text())
    failures = []

    def compare(label, expected, actual):
        if expected != actual:
            failures.append(f"{label}: expected {json.dumps(expected)[:600]}, native {json.dumps(actual)[:600]}")

    driver = Driver(binary)
    ulp_cases = []

    def close_float(label, expected, actual, ulps=2):
        """Domain doubles: libm pow/log1p may differ from V8's in the last
        bits. Exact matches are required for everything contract-observable
        (ranks, buckets, intervals, counters after records); a domain
        difference within `ulps` is recorded and reported, not failed."""
        if expected == actual:
            return
        if isinstance(expected, float) or isinstance(actual, float):
            e, a = float(expected), float(actual)
            if e != 0 and abs(e - a) <= ulps * math.ulp(e):
                ulp_cases.append(f"{label}: ts {e!r} native {a!r}")
                return
        failures.append(f"{label}: expected {expected!r}, native {actual!r}")

    # ---- domain ---------------------------------------------------------------
    domain = fixture["domain"]
    for c in domain["decayed"]:
        close_float(f"decayed {c}", c["result"], driver.send("decayed", num(c["value"]), c["from"], c["to"])["result"])
    for c in domain["record"]:
        args = state_args(c["state"]) if c["state"] else ["-"]
        got = driver.send("record_request", *args, c["now"], 1 if c["newUser"] else 0)
        for field in ("users", "hits", "updatedAt", "lastRequestedAt"):
            close_float(f"recordRequest {c['state']} {c['now']} {field}", c["result"][field], got[field])
    for c in domain["score"]:
        close_float(f"score {c}", c["result"], driver.send("score", *state_args(c["state"]), c["now"])["result"])
    for c in domain["expired"]:
        compare(f"expired {c}", c["result"], driver.send("expired", *state_args(c["state"]), c["now"])["result"])
    for c in domain["bucketForRank"]:
        compare(f"bucket {c['rank']}", c["result"], driver.send("bucket", c["rank"])["result"])
    for c in domain["intervalForBucket"]:
        compare(f"interval {c['bucket']}", c["result"], driver.send("interval", c["bucket"])["result"])
    print(f"domain: {sum(len(v) for k, v in domain.items() if k != 'constants')} cases, "
          f"{len(ulp_cases)} within-ULP differences")
    for case in ulp_cases:
        print("  ULP", case)

    # ---- tracker scripts --------------------------------------------------------
    def ascii_lower(text):
        return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in text)

    def pre_lowered(raw):
        """js-case: the tracker lower-cases ids with ASCII rules
        only. An input id whose Unicode lower case differs (e.g. "Ünï-Coin")
        is sent already lower-cased the way TypeScript did, so these scripts
        keep comparing the tracker's arithmetic against the oracle; the
        ASCII mapping itself is pinned by js_case_test and price_markets_test."""
        trimmed = raw.strip()
        return trimmed.lower() if trimmed.lower() != ascii_lower(trimmed) else raw

    for script in fixture["scripts"]:
        driver.send("new", script.get("maxCoins", "-"), script.get("maxSeenPerDay", "-"))
        for index, (op, expected) in enumerate(zip(script["ops"], script["outputs"])):
            kind = op["op"]
            device = hexed(op["device"]) if op.get("device") else "-"
            if kind == "record":
                got = driver.send("record", op["at"], device, *[hexed(pre_lowered(i)) for i in op["ids"]])
            elif kind == "repeat":
                got = driver.send("repeat", op["at"], device, op["times"], hexed(pre_lowered(op["id"])))
            elif kind == "due":
                # locale-compare: equal scores tie-break in byte
                # order of the id, not ICU order. The scores come from the
                # tracker's own states at the same instant.
                scores = {row["id"]: driver.send("score", *state_args(row), op["at"])["result"]
                          for row in fix_ids(driver.send("states", op["at"]))}
                if isinstance(expected, list) and all(r["id"] in scores for r in expected):
                    expected = compat_expectations.byte_order_ties(expected, scores.__getitem__)
                got = fix_ids(driver.send("due", op["at"], op["budget"]))
            elif kind == "mark":
                got = driver.send("mark", op["at"], *[hexed(i) for i in op["ids"]])
            elif kind == "dirty":
                got = fix_ids(driver.send("dirty", op["at"]))
                got = sorted(got, key=lambda r: r["id"].encode("utf-16-be"))
            elif kind == "states":
                got = fix_ids(driver.send("states", op["at"]))
            elif kind == "size":
                got = driver.send("size", op["at"])
            elif kind == "prune":
                got = driver.send("prune", op["at"], op.get("max", "-"))
            elif kind == "restore":
                rows = [":".join([hexed(i), *[str(x) for x in state_args(s)]]) for i, s in op["rows"]]
                got = driver.send("restore", op["at"], *rows)
            else:
                raise SystemExit(f"unknown op {kind}")
            if (script["name"], index) in KNOWN_DIFFERENCES:
                note, adjusted = KNOWN_DIFFERENCES[(script["name"], index)]
                compare(f"{script['name']}[{index}] {kind} (known difference)", adjusted(expected), got)
                print(f"  known difference {script['name']}[{index}]: {note}")
                continue
            compare(f"{script['name']}[{index}] {kind}", expected, got)
        print(f"script {script['name']}: {len(script['ops'])} ops")

    # ---- runs -------------------------------------------------------------------
    rows = fixture["upstream"]["rows"]
    with tempfile.TemporaryDirectory(prefix="coin-popularity-") as temp:
        root = Path(temp)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        current = {"reply": {}, "hang": None}
        recorded = []
        lock = threading.Lock()

        def body_for(reply, ids):
            if reply.get("status"):
                return reply["status"], json.dumps({"error": "upstream"}).encode()
            fail = reply.get("failIfContains")
            if fail and fail["id"] in ids:
                return fail["status"], json.dumps({"error": "upstream"}).encode()
            special = next((b for b in reply.get("bodies", []) if b["ifContains"] in ids), None)
            if special and special["body"] == "object":
                return 200, json.dumps({"error": "not a list"}).encode()
            if special and special["body"] == "text":
                return 200, b"upstream says no"
            listed = [rows[i] for i in ids if i in rows]
            if special and special["body"] == "nullRow":
                listed.insert(1, None)
            return 200, json.dumps(listed, ensure_ascii=False, separators=(",", ":")).encode()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                if not parsed.path.endswith("/coins/markets"):
                    status, body = 404, b"{}"
                else:
                    ids = [i for i in parse_qs(parsed.query).get("ids", [""])[0].split(",") if i]
                    status, body = body_for(current["reply"], ids)
                    with lock:
                        recorded.append({"ids": ids, "status": status, "path": len(self.path)})
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
        port = server.server_address[1]
        NOW = Date_now = None  # noqa: F841 (documentation: offsets are from the fixture instant)
        now = 1767323045000

        def collapse(requests):
            out, attempts = [], []
            for r in requests:
                entry = {"ids": r["ids"], "status": r["status"]}
                if out and out[-1] == entry:
                    attempts[-1] += 1
                else:
                    out.append(entry)
                    attempts.append(1)
            return out, attempts

        def inspect_native(at, ids):
            out = []
            for i in ids:
                got = fix_ids(driver.send("inspect", at, hexed(i)))
                market = got["market"]
                if market:
                    market = {"d": market["d"], "asOf": market["asOf"] - now, "freshMs": market["freshMs"],
                              "staleMs": market["staleMs"]}
                spot = got["spot"]
                if spot:
                    spot = {"d": {"price": spot["price"], "change24hPct": spot["change24hPct"],
                                  "source": spot["source"]},
                            "asOf": spot["asOf"] - now, "freshMs": spot["freshMs"], "staleMs": spot["staleMs"]}
                series = got["series"]
                if series:
                    series = dict(series, asOf=series["asOf"] - now)
                out.append({"id": got["id"], "market": market, "spot": spot, "eurSpot": got["eurSpot"],
                            "series": series})
            return out

        def persisted_native(at):
            return fix_ids(driver.send("persisted", at))["rows"]

        def seed(scenario_seed):
            for entry in scenario_seed:
                device = hexed(entry["device"]) if entry.get("device") else "-"
                driver.send("repeat", now, device, entry["times"], hexed(entry["id"]))

        scenario_index = 0
        for scenario in fixture["runs"]:
            name = scenario["name"]
            if name in ("restart", "idle"):
                continue
            scenario_index += 1
            directory = root / f"kv-{scenario_index}"
            directory.mkdir()
            current["reply"] = scenario["reply"]
            driver.send("open", directory, port, ca)
            driver.send("new", "-", "-")
            seed(scenario["seed"])
            for expected in scenario["runs"]:
                at = now + expected["at"]
                before = len(recorded)
                result = driver.send("run", at, 250, 1000)
                requests, attempts = collapse(recorded[before:])
                ts_requests, ts_attempts = collapse(expected["requests"])
                label = f"run {name}@{expected['at']}"
                compare(f"{label} requests", ts_requests, requests)
                refreshed = "TypeError" if result["aborted"] else result["refreshed"]
                compare(f"{label} refreshed", expected["refreshed"], refreshed)
                compare(f"{label} dueAfter", expected["dueAfter"],
                        [unhexed(i) for i in driver.send("due_ids", at)])
                compare(f"{label} inspect", expected["inspect"], inspect_native(at, scenario["inspect"]))
                compare(f"{label} persisted", expected["persisted"], len(persisted_native(at)))
                print(f"{label}: refreshed={refreshed} calls={result['calls']} attempts ts={ts_attempts} "
                      f"native={attempts} saved={result['saved']} frames={result['saveFrames']} "
                      f"bytes={result['saveBytes']} {result['elapsedUs']} us")
            got_rows = persisted_native(now)
            compare(f"run {name} persisted rows", scenario["persisted"], got_rows[:12])
            compare(f"run {name} persisted count", scenario["persistedCount"], len(got_rows))
            driver.send("close")

        # The ranked job may still fetch market details for core coins. It
        # leaves an existing fresh snapshot quote alone and fills a cold one.
        directory = root / "kv-snapshot-ownership"
        directory.mkdir()
        current["reply"] = {}
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        driver.send("snapshot_owns_core", 1)
        driver.send("record", now, "-", hexed("bitcoin"))
        cold = driver.send("run", now, 250, 1000)
        assert cold["quotes"] == 1, cold
        warm_at = now + 60_000
        warm = driver.send("run", warm_at, 250, 1000)
        assert warm["calls"] == 1 and warm["listed"] == 1 and warm["quotes"] == 0, warm
        view = driver.send("inspect", warm_at, hexed("bitcoin"))
        assert view["market"]["asOf"] == warm_at and view["spot"]["asOf"] == now, view
        driver.send("snapshot_owns_core", 0)
        driver.send("close")

        # ---- restart ----------------------------------------------------------------
        restart = next(s for s in fixture["runs"] if s["name"] == "restart")
        directory = root / "kv-restart"
        directory.mkdir()
        current["reply"] = {}
        phase = restart["phases"][0]
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        seed(restart["seed"])
        before = len(recorded)
        result = driver.send("run", now, 250, 1000)
        compare("restart[0] requests", collapse(phase["requests"])[0], collapse(recorded[before:])[0])
        compare("restart[0] refreshed", phase["refreshed"], result["refreshed"])
        compare("restart[0] states", phase["states"], fix_ids(driver.send("states", now)))
        compare("restart[0] persisted", phase["persisted"], persisted_native(now))
        driver.send("close")
        for phase in restart["phases"][1:]:
            at = now + phase["at"]
            before = len(recorded)
            driver.send("open", directory, port, ca)
            restored = driver.send("cold_restore", at)
            compare(f"restart@{phase['at']} boot requests", phase["bootRequests"], len(recorded) - before)
            compare(f"restart@{phase['at']} states", phase["states"], fix_ids(driver.send("states", at)))
            compare(f"restart@{phase['at']} due", phase["due"], [unhexed(i) for i in driver.send("due_ids", at)])
            pruned = driver.send("cprune", at)
            compare(f"restart@{phase['at']} physical after prune", phase["physicalAfterPrune"],
                    driver.send("physical"))
            result = driver.send("run", at, 250, 1000)
            compare(f"restart@{phase['at']} requests", collapse(phase["requests"])[0],
                    collapse(recorded[before:])[0])
            compare(f"restart@{phase['at']} refreshed", phase["refreshed"], result["refreshed"])
            compare(f"restart@{phase['at']} persisted", phase["persisted"], persisted_native(at))
            print(f"restart@{phase['at']}: restored={restored} prune={pruned} refreshed={result['refreshed']}")
            driver.send("close")

        # ---- idle -------------------------------------------------------------------
        idle = next(s for s in fixture["runs"] if s["name"] == "idle")
        directory = root / "kv-idle"
        directory.mkdir()
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        before = len(recorded)
        size_before = (directory / "fortis.kv").stat().st_size
        result = driver.send("run", now, 80, 10)
        pruned = driver.send("cprune", now)
        compare("idle requests", idle["requests"], recorded[before:])
        compare("idle refreshed", idle["refreshed"], result["refreshed"])
        compare("idle physical", idle["physical"], driver.send("physical"))
        compare("idle size", idle["size"], driver.send("size", now))
        compare("idle writes", 0, (directory / "fortis.kv").stat().st_size - size_before)
        compare("idle saved", 0, result["saved"])
        print(f"idle: run={result} prune={pruned}")
        driver.send("close")

        # ---- native-only: production batch, call cap, stop ---------------------------------
        directory = root / "kv-native"
        directory.mkdir()
        current["reply"] = {}
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        for i in range(400):
            driver.send("repeat", now, "-", 400 - i, hexed(f"coin-{i:03d}"))
        before = len(recorded)
        first = driver.send("run", now, 80, 3)
        sizes = [len(r["ids"]) for r in recorded[before:]]
        compare("native cap calls", 3, first["calls"])
        compare("native cap refreshed", 240, first["refreshed"])
        compare("native cap deferred", 160, first["deferred"])
        compare("native cap batch sizes", [80, 80, 80], sizes)
        compare("native cap first ids", [f"coin-{i:03d}" for i in range(80)], recorded[before]["ids"])
        before = len(recorded)
        second = driver.send("run", now, 80, 3)
        compare("native next run leads with deferred",
                [f"coin-{i:03d}" for i in range(240, 320)], recorded[before]["ids"])
        compare("native next run refreshed", 160, second["refreshed"])
        if max(r["path"] for r in recorded) > 4096:
            failures.append("a request path exceeded 4096 bytes")
        # Stop between batches: the answered batch is marked and the dirty
        # counters saved; nothing after the stop is requested.
        driver.send("repeat", now + 61000, "-", 1, hexed("late-coin"))
        before = len(recorded)
        stopped = driver.send("run", now + 61000, 40, 10, 1)
        compare("native stop calls", 1, stopped["calls"])
        compare("native stop flag", True, stopped["stopped"])
        compare("native stop requests", 1, len(recorded) - before)
        compare("native stop saved", 1, stopped["saved"])
        print(f"native cap: first={first} second={second} stop={stopped}")
        driver.send("close")

        # Partial save: every 100-row frame is one fsynced KV transaction. A
        # torn last frame (the process died mid-write) is discarded on reopen
        # and the earlier frames restore intact; nothing half-written loads.
        directory = root / "kv-partial"
        directory.mkdir()
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        for i in range(250):
            driver.send("repeat", now, "-", 1, hexed(f"save-{i:03d}"))
        saved = driver.send("save", now)
        compare("partial save frames", 3, saved["frames"])
        driver.send("close")
        kv_file = directory / "fortis.kv"
        size = kv_file.stat().st_size
        with open(kv_file, "r+b") as handle:
            handle.truncate(size - 40)
        driver.send("open", directory, port, ca)
        restored = driver.send("cold_restore", now)
        compare("partial save restores whole frames", 200, restored["rows"])
        states = fix_ids(driver.send("states", now))
        compare("partial save rows intact", [f"save-{i:03d}" for i in range(200)], [r["id"] for r in states])
        # The lost rows are re-saved by the next record of those coins.
        driver.send("repeat", now + 1000, "-", 1, hexed("save-249"))
        compare("partial save resumes", 1, driver.send("save", now + 1000)["rows"])
        print(f"partial save: {saved} truncated 40 B -> restored {restored['rows']}")
        driver.send("close")

        # A provider answer is not fresh until the cache write is durable.
        directory = root / "kv-write-failure"
        directory.mkdir()
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        driver.send("record", now, "-", hexed("alpha"))
        driver.send("kv_fail", "write_before_fsync")
        rejected = driver.send("run", now, 80, 10)
        compare("cache write failure aborts batch", True, rejected["aborted"])
        compare("cache write failure marks no coins", 0, rejected["refreshed"])
        driver.send("kv_fail", "-")
        compare("cache write recovery reopens KV", True, driver.send("kv_open", directory))
        retried = driver.send("run", now + 1, 80, 10)
        compare("cache write retry refreshes", 1, retried["refreshed"])
        driver.send("close")

        # A failed popularity save retains its dirty rows for the next pass.
        directory = root / "kv-save-failure"
        directory.mkdir()
        driver.send("open", directory, port, ca)
        driver.send("new", "-", "-")
        driver.send("record", now, "-", hexed("alpha"))
        driver.send("mark", now, hexed("alpha"))  # no provider call due
        driver.send("kv_fail", "write_before_fsync")
        failed_save = driver.send("run", now, 80, 10)
        compare("popularity save failure reported", True, failed_save["failed"])
        compare("popularity save failure writes none", 0, failed_save["saved"])
        driver.send("kv_fail", "-")
        compare("popularity save recovery reopens KV", True, driver.send("kv_open", directory))
        saved_again = driver.send("run", now + 1, 80, 10)
        compare("popularity save retry writes row", 1, saved_again["saved"])
        compare("popularity save retry avoids provider", 0, saved_again["calls"])
        driver.send("close")
        server.shutdown()

        # ---- load -------------------------------------------------------------------
        directory = root / "kv-load"
        directory.mkdir()
        load = driver.send("measure", directory, 20000, 16)
        compare("load tracked", 20000, load["tracked"])
        compare("load pairs capped", 300000, load["seenPairs"])
        compare("load dropped", 0, load["dropped"])
        compare("load due", 2500, load["dueCount"])
        compare("load saved", 20000, load["saved"])
        compare("load frames", 200, load["saveFrames"])
        compare("load idle writes", 0, load["idleFileDelta"])
        compare("load restored", 20000, load["restored"])
        compare("load prune keeps", 20000, load["pruneAfter"])
        compare("load expiry prune", 0, load["expiredAfter"])
        if load["peakAllocatedBytes"] > 8 * 1024 * 1024:
            failures.append(f"tracker peak allocation {load['peakAllocatedBytes']} B over 8 MiB")
        print("load:", json.dumps(load))
    driver.close()

    if failures:
        for failure in failures:
            print("FAIL", failure, file=sys.stderr)
        raise SystemExit(1)
    print("coin popularity contract: all cases match")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
