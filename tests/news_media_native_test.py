#!/usr/bin/env python3
"""Native-only functional checks for the news and media service,
beyond the feed replay:

 1. a cold 1.5 MiB image requested by 12 clients at once: one upstream fetch
    (two media jobs: the second re-reads the stored bytes), every client gets
    the exact bytes through the bounded response handoff;
 2. blob retention: chunks written at fetch time expire seven days later
    unless a view at least a day after the last chunk write refreshes them;
    an expired blob is re-fetched, never served;
 3. the scheduler: with manual jobs off the first ingest runs at startup;
 4. the feed transport: redirects are followed (five hops at most), an
    oversized document is refused without a retry, all feeds down still
    answers the stored wire;
 5. eight simultaneous reaction jobs holding near-ceiling sample arrays
    (about 4,000 samples for each of five coins) stay within the documented
    bound: the process RSS growth is measured and must stay under 64 MiB
    (the documented worst case is 20 MiB; the target host is 512 MiB);
 6. startup refusals: FORTIS_NATIVE_NEWS_CANARY=1 without, or with an
    unreadable, CA bundle exits 2; the default image has no news routes.
Usage: news_media_native_test.py <test binary> <production binary>
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from email.utils import formatdate
from pathlib import Path

import news_route_harness as harness

T = 1_792_713_237_000  # 2026-10-22T23:53:57Z, the replay's first ingest
DAY = 86_400_000
H = 3_600_000
TOKEN = "fintel_dev_" + "A" * 43
SEC = "https://www.sec.gov/news/pressreleases.rss"


def rss(items):
    body = "".join(
        f"<item><title>{title}</title><link>{link}</link><guid>{link}</guid>"
        f"<pubDate>{formatdate(at / 1000, usegmt=True)}</pubDate>"
        f"<description>{title}.</description>{extra}</item>"
        for title, link, at, extra in items)
    return (f'<?xml version="1.0"?><rss version="2.0" xmlns:media="http://search.yahoo.com/mrss/">'
            f"<channel><title>t</title>{body}</channel></rss>").encode()


def media(url):
    return f'<media:content url="{url}" medium="image"/>'


class Upstream(harness.Upstream):
    def __init__(self, root):
        self.routes = {}
        self.delays = {}
        super().__init__(root, self.answer, delay=self.wait)

    def wait(self, url):
        for prefix, seconds in self.delays.items():
            if url.startswith(prefix):
                time.sleep(seconds)

    def answer(self, url, _headers):
        route = self.routes.get(url)
        if route is None:
            for prefix, value in self.routes.items():
                if prefix.endswith("*") and url.startswith(prefix[:-1]):
                    route = value
                    break
        if route is None:
            return 404, {}, b"not found"
        return route(url) if callable(route) else route

    def count(self, url):
        return sum(1 for seen in self.calls() if seen == url)


def get(server, path, accept="application/json", auth=True, token=TOKEN):
    headers = {"Accept": accept}
    if auth:
        headers["Authorization"] = "Bearer " + token
    return server.request("GET", path, headers=headers)


def media_key(url):
    import hashlib
    return hashlib.sha256(url.encode()).hexdigest()[:32]


def coalescing_and_retention(binary, root):
    up = Upstream(root)
    clock = root / "clock"
    kv = root / "kv"
    kv.mkdir()
    large = harness.image("png", 1_536_000, 7)
    small = harness.image("png", 4_000, 9)
    items = [
        ("Orchard harvest lifts apple growers", "https://www.sec.gov/a", T - H,
         media("https://media.example.com/large.png")),
        ("Glacier retreat alarms mountain guides", "https://www.sec.gov/b", T - H,
         media("https://media.example.com/exp.png")),
        ("Lighthouse keeper logs record storm", "https://www.sec.gov/c", T - H,
         media("https://media.example.com/ref.png")),
    ]
    up.routes[SEC] = (200, {"Content-Type": "application/rss+xml"}, rss(items))
    up.routes["https://media.example.com/large.png"] = (200, {"Content-Type": "image/png"}, large)
    up.routes["https://media.example.com/exp.png"] = (200, {"Content-Type": "image/png"}, small)
    up.routes["https://media.example.com/ref.png"] = (200, {"Content-Type": "image/png"}, small)
    up.delays["https://media.example.com/large.png"] = 1.5
    harness.set_clock(clock, T)
    server = harness.Server(binary, kv, clock, up)
    try:
        result = server.probe("/probe/test-news-ingest")
        assert result["ok"] and result["stored"] == 3 and result["feedsFailed"] == 5, result

        # 1. twelve concurrent cold requests, one fetch, exact bytes.
        path = "/v1/media/" + media_key("https://media.example.com/large.png")
        answers = [None] * 12
        def fetch(index):
            answers[index] = get(server, path, accept="image/*", auth=False)
        threads = [threading.Thread(target=fetch, args=(i,)) for i in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        assert all(status == 200 and body == large and headers["content-type"] == "image/png"
                   and headers["content-length"] == str(len(large))
                   for status, headers, body in answers), [a[0] for a in answers]
        assert up.count("https://media.example.com/large.png") == 1, up.calls()
        print("  12 concurrent cold 1.5 MiB requests: 1 upstream fetch, 12 exact replies")

        # 2. retention. exp: fetched at T+1h, viewed at T+12h (record refreshed,
        # chunks not: their remaining life exceeds six days), so the chunks
        # expire at T+7d+1h while the record lives to T+7d+12h.
        exp = "/v1/media/" + media_key("https://media.example.com/exp.png")
        ref = "/v1/media/" + media_key("https://media.example.com/ref.png")
        harness.set_clock(clock, T + H)
        assert get(server, exp, auth=False)[0] == 200
        assert get(server, ref, auth=False)[0] == 200
        harness.set_clock(clock, T + 12 * H)
        assert get(server, exp, auth=False)[0] == 200
        # ref: viewed two days after its chunk write -> chunks rewritten.
        harness.set_clock(clock, T + 2 * DAY + H)
        assert get(server, ref, auth=False)[0] == 200
        assert up.count("https://media.example.com/exp.png") == 1
        assert up.count("https://media.example.com/ref.png") == 1
    finally:
        server.stop()
    # A restart in between: records and blobs are durable.
    server = harness.Server(binary, kv, clock, up)
    try:
        harness.set_clock(clock, T + 2 * DAY + 2 * H)
        status, _, body = get(server, path, auth=False)
        assert status == 200 and body == large
        assert up.count("https://media.example.com/large.png") == 1
        harness.set_clock(clock, T + 7 * DAY + 2 * H)
        status, _, body = get(server, exp, auth=False)
        assert status == 200 and body == small
        assert up.count("https://media.example.com/exp.png") == 2, "expired chunks must be re-fetched"
        status, _, body = get(server, ref, auth=False)
        assert status == 200 and body == small
        assert up.count("https://media.example.com/ref.png") == 1, "refreshed chunks must survive"
        print("  blob retention: unrefreshed chunks re-fetched after 7 days, refreshed ones served; "
              "restart keeps records and blobs")
        # Past the record's own expiry nothing is registered any more.
        harness.set_clock(clock, T + 30 * DAY)
        status, _, _ = get(server, "/v1/media/" + media_key("https://media.example.com/large.png"),
                           auth=False)
        assert status == 404
        assert up.count("https://media.example.com/large.png") == 1
    finally:
        server.stop()
        up.close()


def scheduler_and_feeds(binary, root):
    up = Upstream(root)
    clock = root / "clock2"
    kv = root / "kv2"
    kv.mkdir()
    doc = rss([("Bakery chain trials sourdough subscription", "https://www.sec.gov/d", T - H, "")])
    up.routes[SEC] = (301, {"Location": "/moved.rss"}, b"")
    up.routes["https://www.sec.gov/moved.rss"] = (302, {"Location": "https://www.sec.gov/final.rss"}, b"")
    up.routes["https://www.sec.gov/final.rss"] = (200, {"Content-Type": "application/rss+xml"}, doc)
    harness.set_clock(clock, T)
    # 3. the scheduler runs the first ingest immediately.
    server = harness.Server(binary, kv, clock, up, env={"FORTIS_TEST_NEWS_MANUAL_JOBS": "0"})
    try:
        deadline = time.time() + 60
        while server.probe("/probe/test-news-jobs")["ingestRuns"] < 1:
            assert time.time() < deadline, "the startup ingest never ran"
            time.sleep(.2)
        status, _, body = get(server, "/v1/news")
        items = json.loads(body)["items"]
        assert status == 200 and [i["headline"] for i in items] == [
            "Bakery chain trials sourdough subscription"], body[:300]
        assert up.count("https://www.sec.gov/final.rss") == 1
        print("  scheduler: startup ingest ran; two feed redirects followed")
    finally:
        server.stop()
    server = harness.Server(binary, kv, clock, up)
    try:
        # 4. a redirect loop fails after five hops; an oversized feed is not retried.
        up.routes[SEC] = (301, {"Location": "/loop.rss"}, b"")
        up.routes["https://www.sec.gov/loop.rss"] = (301, {"Location": "/news/pressreleases.rss"}, b"")
        up.routes["https://www.federalreserve.gov/feeds/press_all.xml"] = (
            200, {"Content-Type": "application/rss+xml"}, b"<rss>" + b" " * (2 * 1024 * 1024 + 1))
        before = len(up.calls())
        harness.set_clock(clock, T + 10 * 60_000)
        result = server.probe("/probe/test-news-ingest")
        assert result["ok"] and result["fetched"] == 0 and result["feedsFailed"] == 6, result
        calls = up.calls()[before:]
        assert calls.count(SEC) + calls.count("https://www.sec.gov/loop.rss") == 6, calls
        assert calls.count("https://www.federalreserve.gov/feeds/press_all.xml") == 1, calls
        # Every feed down: the stored wire is still answered.
        status, _, body = get(server, "/v1/news?limit=5")
        assert status == 200 and len(json.loads(body)["items"]) == 1
        print("  feeds: redirect loop stops at 5 hops, oversized document not retried, "
              "outage keeps the stored wire")
    finally:
        server.stop()
        up.close()


COINS = ["ethereum", "solana", "cardano", "dogecoin", "litecoin", "stellar", "algorand",
         "cosmos", "aptos", "zcash", "vechain", "ripple"]
UNIQUE = ["amber", "basalt", "cobalt", "dune", "ember", "fjord", "garnet", "harbor", "iris",
          "juniper", "kelp", "lagoon", "marble", "nectar", "onyx", "prairie", "quartz", "reef",
          "saffron", "tundra", "umber", "violet", "willow", "yarrow", "zinnia", "atlas", "birch",
          "cedar", "delta", "estuary", "fern", "glade", "heath", "inlet", "jade", "knoll", "lichen",
          "meadow", "nimbus", "oasis", "pebble", "quill", "ridge", "sorrel", "thistle", "upland",
          "valley", "wharf"]


def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def reaction_memory(binary, root):
    up = Upstream(root)
    clock = root / "clock3"
    kv = root / "kv3"
    kv.mkdir()
    day0 = (T // DAY) * DAY
    published = day0 + DAY + 10 * 60_000   # 00:10 on day 1: a three-day window
    now = published + DAY + 10 * 60_000
    stories = []
    for i in range(8):
        coins = [COINS[(i + j) % len(COINS)] for j in range(4)]
        words = UNIQUE[i * 6:(i + 1) * 6]
        title = " ".join(c.capitalize() for c in coins) + " " + " ".join(words)
        stories.append((title, f"https://www.sec.gov/r{i}", published, ""))
    up.routes[SEC] = (200, {"Content-Type": "application/rss+xml"}, rss(stories))
    # Day 2 of every coin is answered slowly, so all eight jobs hold their
    # day 0 and day 1 samples at once.
    up.routes["https://api.coingecko.com/*"] = (200, {"Content-Type": "application/json"},
                                                b'{"prices":[]}')
    up.delays["https://api.coingecko.com/"] = 6.0
    harness.set_clock(clock, published + 60_000)
    server = harness.Server(binary, kv, clock, up)
    try:
        result = server.probe("/probe/test-news-ingest")
        assert result["stored"] == 8, result
        harness.set_clock(clock, now)
        step = 22_000  # about 3,980 in-window samples per coin: the 4,096 ceiling
        for coin in COINS + ["bitcoin"]:
            for day, start, count in ((day0, published - 30 * 60_000, 30 * 60_000 // step),
                                      (day0 + DAY, day0 + DAY, DAY // step)):
                seeded = server.probe(f"/probe/test-news-seed-samples?coin={coin}&day={day}"
                                      f"&from={start}&step={step}&count={count}&base=1000")
                assert seeded["ok"], seeded
        status, _, body = get(server, "/v1/news?limit=20")
        ids = [item["id"] for item in json.loads(body)["items"]]
        tickers = [len(item["tickers"]) for item in json.loads(body)["items"]]
        assert len(ids) == 8 and all(count == 4 for count in tickers), (ids, tickers)
        baseline = rss_kib(server.process.pid)
        peak = baseline
        answers = [None] * 8
        def react(index):
            answers[index] = get(server, f"/v1/news/{ids[index]}/reactions",
                                 token="fintel_dev_" + harness.TOKEN_LETTERS[index + 1] * 43)
        threads = [threading.Thread(target=react, args=(i,)) for i in range(8)]
        for thread in threads:
            thread.start()
        while any(thread.is_alive() for thread in threads):
            peak = max(peak, rss_kib(server.process.pid))
            time.sleep(.05)
        statuses = [a[0] for a in answers]
        assert all(status == 200 for status in statuses), statuses
        held = [len(json.loads(a[2])["assets"]) for a in answers]
        assert held == [4] * 8, held
        growth_mib = (peak - baseline) / 1024
        print(f"  8 simultaneous reaction jobs (5 coins x ~3,980 samples each): RSS "
              f"{baseline / 1024:.1f} -> peak {peak / 1024:.1f} MiB (+{growth_mib:.1f} MiB)")
        assert growth_mib < 64, growth_mib
    finally:
        server.stop()
        up.close()


def startup_refusals(production):
    def run(env):
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("FORTIS_") and k not in ("PORT",)}
        environment.update(env)
        return subprocess.run([production, "not-a-port"], env=environment, capture_output=True,
                              text=True, timeout=30)
    missing = run({"FORTIS_NATIVE_NEWS_CANARY": "1"})
    assert missing.returncode == 2 and "requires FORTIS_CA_BUNDLE_PATH" in missing.stderr, missing
    unreadable = run({"FORTIS_NATIVE_NEWS_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": "/nonexistent.pem"})
    assert unreadable.returncode == 2 and "Invalid CA bundle" in unreadable.stderr, unreadable
    print("  startup: news canary refused without a CA bundle and with an unreadable one")


def default_off(production, root):
    kv = root / "kv4"
    kv.mkdir()
    port = harness.free_port()
    environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_")}
    environment.update({"FORTIS_KV_DIR": str(kv), "PORT": str(port)})
    process = subprocess.Popen([production, str(port)], env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(200):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    break
            except OSError:
                time.sleep(.03)
        probe = harness.Server.__new__(harness.Server)
        probe.port = port
        for path in ("/v1/news", f"/v1/media/{'0' * 32}", f"/v1/news/{'0' * 26}/reactions"):
            status, _, _ = probe.request("GET", path, headers={"Authorization": "Bearer " + TOKEN})
            assert status == 404, (path, status)
        print("  default image: the four news/media routes are absent (404)")
    finally:
        process.terminate()
        process.wait(timeout=10)


def main():
    binary, production = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        coalescing_and_retention(binary, root)
        scheduler_and_feeds(binary, root)
        reaction_memory(binary, root)
        startup_refusals(production)
        default_off(production, root)
    print("news/media native checks passed")


if __name__ == "__main__":
    main()
