#!/usr/bin/env python3
"""Pinned core shares the existing quota and leaves the demand-ranked tail due."""
import os
import sys
import tempfile
from pathlib import Path
from coin_popularity_test import Driver, hexed
from coin_popularity_service_test import Upstream

CORE = {"bitcoin", "ethereum", "binancecoin", "ripple", "solana", "algorand"}
os.environ["FORTIS_TEST_PINNED_PRICE_CORE"] = "1"
with tempfile.TemporaryDirectory(prefix="fortis-price-core-") as root:
    Path(root + "/kv").mkdir()
    upstream = Upstream(Path(root))
    driver = Driver(sys.argv[1])
    try:
        driver.send("open", root + "/kv", upstream.port, upstream.ca)
        driver.send("new", "-", "-")
        now = 1767323045000
        ids = ["tail-%03d" % n for n in range(90)]
        driver.send("record", now, "-", *map(hexed, ids))
        first = driver.send("run", now, 80, 1)
        assert first["calls"] == 1 and first["quotes"] == 80, first
        calls = upstream.matching("/coins/markets", 0)
        assert len(calls) == 1 and CORE.issubset(set(calls[0][3])), calls
        # Pins consume six IDs inside one call, never add synthetic demand.
        assert driver.send("size", now) == 90
        second = driver.send("run", now, 80, 1)
        assert second["calls"] == 1 and second["quotes"] == 16, second
        third = driver.send("run", now, 80, 1)
        assert third["calls"] == 0, third
        # 60-second lookahead refreshes core before its ~90-second envelope ends.
        later = driver.send("run", now + 60000, 80, 1)
        assert later["calls"] == 1, later
        calls = upstream.matching("/coins/markets", 0)
        assert CORE.issubset(set(calls[-1][3])), calls[-1]
        # Fresh pin-only work yields on a busy lane, while ranked work keeps
        # its admission. Already-stale pins can enter the bounded lane.
        driver.send("close")
        driver.send("open", root + "/kv", upstream.port, upstream.ca)
        driver.send("new", "-", "-")
        driver.send("record", now + 120000, "-", hexed("tail-extra"))
        busy = driver.send("run_busy", now + 120000)
        assert busy["calls"] == 2 and busy["laneRejected"] == 1, busy
        assert busy["quotes"] == 1 and busy["deferred"] == 6, busy
        assert upstream.matching("/coins/markets", 0)[-1][3] == ["tail-extra"]
        driver.send("close")
        driver.send("open", root + "/kv", upstream.port, upstream.ca)
        driver.send("new", "-", "-")
        stale = driver.send("run_busy", now + 240000)
        assert stale["calls"] == 1 and stale["laneRejected"] == 0, stale
        assert stale["quotes"] == 6, stale
        print("pinned core quota, warm cache, demand tail, lane contention and freshness lookahead passed")
    finally:
        driver.close()
        upstream.server.shutdown()
        upstream.server.server_close()
