#!/usr/bin/env python3
"""Exercise the native node's Portfolio price routes with synthetic devices."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from http.client import HTTPConnection, HTTPSConnection, HTTPException
import ipaddress
import json
import math
import re
import ssl
import sys
from threading import Event
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPSHandler, ProxyHandler, Request, build_opener
from weakref import ref

from public_price_device_probe import (
    DEFAULT_IDS, MAX_REPLY, NoRedirect, ProbeError, base_url, enroll, enrollment_tunnel_opener, price_result,
    fields, text_field, error_code as response_error,
)

CORE = frozenset(("bitcoin", "ethereum", "binancecoin", "ripple", "solana", "algorand"))
SPOT_PATH = "/v1/prices?" + urlencode({"fiat": "usd", "ids": ",".join(sorted(CORE) + list(DEFAULT_IDS))})
# Challenge and enrollment consume one unit each from the same 10/min bucket.
ENROLLMENT_BATCH = 5
ENROLLMENT_WINDOW_S = 60
ENROLLMENT_EXTRA_WINDOWS = 2


@dataclass
class Sample:
    status: int
    ms: float
    size: int = 0
    ids: frozenset = frozenset()
    expected_ids: frozenset = frozenset()
    result: str = "http_error"
    done: float = 0.0
    error_code: str = ""
    stale_ids: int = 0
    missing_ids: int = 0
    transport_failure: str = ""
    milestones_ms: dict = field(default_factory=dict)
    local_port: int = 0

    @property
    def ok(self):
        return self.result == "ok"


def summarize(samples):
    times = [s.ms for s in samples if s.ok]
    handshakes = [s.milestones_ms["tls"] - s.milestones_ms["tcp"]
                  for s in samples if "tls" in s.milestones_ms]
    result = {
        "requests": len(samples),
        "status": dict(Counter(str(s.status) if s.status else "transport" for s in samples)),
        "outcome": dict(Counter(s.result for s in samples)),
        "all_latency_ms": percentiles([s.ms for s in samples if s.result != "missing_etag"]),
        "successful_latency_ms": percentiles(times),
        "body_bytes": sum(s.size for s in samples),
        "transport_failures": dict(Counter(s.transport_failure for s in samples if s.transport_failure)),
        "tls_handshake_ms": percentiles(handshakes),
        "milestones_ms": {name: percentiles([s.milestones_ms[name] for s in samples
                                             if name in s.milestones_ms])
                          for name in ("tcp", "tls", "headers", "body")
                          if any(name in s.milestones_ms for s in samples)},
    }
    expected = set().union(*(sample.expected_ids for sample in samples))
    if expected:
        result['fresh_by_id'] = {coin: sum(coin in sample.ids for sample in samples) for coin in sorted(expected)}
        result['unavailable_by_id'] = {
            coin: sum(coin in sample.expected_ids and coin not in sample.ids for sample in samples)
            for coin in sorted(expected)
        }
    return result


def percentiles(times):
    times = sorted(times)
    if not times:
        return None
    return {f"p{p}": round(times[math.ceil(len(times) * p / 100) - 1], 1)
            for p in (50, 95, 99, 100)}


def snapshot_ids(payload, now_ms, *, details=False):
    body = json.loads(payload)
    if not isinstance(body, dict) or body.get("schemaVersion") != 1 or body.get("fiat") != "usd":
        raise ValueError("snapshot schema")
    quotes, missing = body["quotes"], body["missing"]
    if not isinstance(quotes, list) or not isinstance(missing, list):
        raise ValueError("snapshot shape")
    if body.get("coverage") != {"available": len(quotes), "requested": len(CORE)}:
        raise ValueError("snapshot coverage")
    seen, fresh = set(), set()
    for row in quotes:
        coin = row["id"]
        if coin not in CORE or coin in seen:
            raise ValueError("snapshot ID")
        seen.add(coin)
        price, expiry, cached_at = row["price"], row["freshUntilMs"], row["cacheAsOfMs"]
        observed = row.get("providerObservedAtMs")
        observed = cached_at if observed is None else observed
        if (type(price) not in (float, int) or not math.isfinite(price) or price <= 0
                or type(expiry) is not int or type(cached_at) is not int or type(observed) is not int
                or not 0 < observed <= now_ms + 30_000 or not 0 < cached_at < expiry
                or type(row["stale"]) is not bool):
            raise ValueError("snapshot quote")
        if not row["stale"] and now_ms < expiry and now_ms - observed < 300_000:
            fresh.add(coin)
    missing_ids = set(missing)
    if len(missing_ids) != len(missing) or seen & missing_ids or seen | missing_ids != CORE:
        raise ValueError("snapshot ID coverage")
    ids = frozenset(fresh)
    return (ids, len(seen - fresh), len(missing_ids)) if details else ids


class TimedHTTPSHandler(HTTPSHandler):
    """Observe fresh direct HTTPS connections without changing urllib's HTTP policy."""

    def __init__(self, context, client):
        super().__init__(context=context)
        self.context, self.client = context, ref(client)

    def https_open(self, request):
        client, context = self.client(), self.context
        if client is None:
            return super().https_open(request)

        class Connection(HTTPSConnection):
            def connect(self):
                HTTPConnection.connect(self)
                client.local_port = self.sock.getsockname()[1]
                client.mark("tcp", "tls")
                self.sock = context.wrap_socket(self.sock, server_hostname=self.host)
                client.mark("tls", "headers")

            def getresponse(self):
                response = super().getresponse()
                client.mark("headers", "body")
                return response

        return self.do_open(Connection, request, context=context)


class Client:
    def __init__(self, target, context, timeout, token=None, benchmark_request_id=None):
        self.target, self.timeout, self.token = target, timeout, token
        self.benchmark_request_id = benchmark_request_id
        self.started = None
        self.opener = build_opener(ProxyHandler({}), TimedHTTPSHandler(context, self), NoRedirect())

    def mark(self, milestone, next_phase):
        if self.started is not None:
            self.milestones_ms[milestone] = (time.monotonic() - self.started) * 1000
            self.phase = next_phase

    def trace(self):
        return {"milestones_ms": self.milestones_ms, "transport_failure": self.transport_failure,
                "local_port": self.local_port}

    def request(self, path, *, method="GET", accept="application/json", extra=None):
        headers = {"Accept": accept}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if self.benchmark_request_id:
            headers["X-Fortis-Benchmark"] = self.benchmark_request_id
        if extra:
            headers.update(extra)
        started = time.monotonic()
        self.started, self.phase = started, "tcp" if self.target.startswith("https://") else "http"
        self.milestones_ms, self.transport_failure = {}, ""
        self.local_port = 0
        req = Request(self.target + path, headers=headers, method=method)
        try:
            with self.opener.open(req, timeout=self.timeout) as response:
                body = response.read(MAX_REPLY + 1)
                self.mark("body", "")
                return response.status, response.headers, body, started
        except HTTPError as error:
            with error:
                try:
                    body = error.read(1024)
                    self.mark("body", "")
                except (OSError, HTTPException) as failure:
                    self.transport_failure = "body:" + type(failure).__name__
                    body = b""
                return error.code, error.headers, body, started
        except (URLError, OSError, HTTPException) as failure:
            reason = failure.reason if isinstance(failure, URLError) else failure
            self.transport_failure = self.phase + ":" + type(reason).__name__
            return 0, {}, b"", started

    def measure(self, route, conditional=False):
        path = "/v1/prices/snapshot" if route == "snapshot" else SPOT_PATH
        expected = CORE if route == "snapshot" else CORE | set(DEFAULT_IDS)
        accept = "application/json" if route == "snapshot" else "application/protobuf"
        status, headers, payload, start = self.request(path, accept=accept)
        done = time.monotonic()
        sample = Sample(status, (done - start) * 1000, len(payload), expected_ids=frozenset(expected), done=done, **self.trace())
        check = None
        if status != 200:
            sample.error_code = response_error(payload)
            sample.result = (
                "rate_limited" if status == 429 else "http_error" if status else "transport_error"
            )
            return sample, check
        try:
            if len(payload) > MAX_REPLY:
                raise ValueError("response too large")
            content_type = headers.get_content_type()
            if route == "snapshot":
                if content_type != "application/json":
                    raise ValueError("snapshot content type")
                sample.ids, sample.stale_ids, sample.missing_ids = snapshot_ids(
                    payload, time.time_ns() // 1_000_000, details=True)
            else:
                if content_type != "application/protobuf" or text_field(payload, 1) != "usd":
                    raise ValueError("spot representation")
                prices, missing, invalid = price_result(payload, list(expected))
                if len(prices) + len(missing) + len(invalid) != len(expected):
                    raise ValueError("spot ID coverage")
                stale = set()
                for number, wire, entry in fields(payload):
                    if number != 2 or wire != 2:
                        continue
                    quote = next(value for n, w, value in fields(entry) if n == 2 and w == 2)
                    if any(n == 5 and w == 0 and value for n, w, value in fields(quote)):
                        stale.add(text_field(entry, 1))
                sample.ids = frozenset(prices.keys() - stale)
                sample.stale_ids = len(stale)
                sample.missing_ids = len(missing) + len(invalid)
            sample.result = "ok" if sample.ids == expected else "incomplete"
        except (ProbeError, ValueError, KeyError, TypeError, OverflowError):
            sample.result = "invalid_response"
            sample.ids = frozenset()
        if conditional and route == "snapshot" and sample.ok:
            etag = headers.get("ETag")
            if not etag:
                return sample, Sample(0, 0, result="missing_etag")
            code, reply, data, begin = self.request(path, extra={"If-None-Match": etag})
            # A quote can legitimately refresh between the two GETs.
            result = "invalid_response"
            if code == 304 and not data and reply.get("ETag") == etag:
                result = "ok"
            elif code == 200 and reply.get("ETag") and reply.get("ETag") != etag:
                try:
                    snapshot_ids(data, time.time_ns() // 1_000_000)
                    result = "changed"
                except (ValueError, KeyError, TypeError, OverflowError):
                    pass
            elif code == 429:
                result = "rate_limited"
            elif not code:
                result = "transport_error"
            check = Sample(code, (time.monotonic() - begin) * 1000, len(data), result=result, **self.trace())
        return sample, check


def enroll_user(target, context, timeout, tunnel_port=None):
    client = Client(target, context, timeout)
    start = time.monotonic()
    try:
        enrollment_opener = enrollment_tunnel_opener(target, context, tunnel_port) if tunnel_port else client.opener
        client.token = enroll(enrollment_opener, target, app_version="portfolio-load-probe", timeout=timeout)
        return client, Sample(201, (time.monotonic() - start) * 1000, result="ok")
    except (ProbeError, OSError, HTTPException) as error:
        match = re.search(r"HTTP (\d{3})", str(error))
        status = int(match[1]) if match else 0
        return None, Sample(status, (time.monotonic() - start) * 1000,
                            result="rate_limited" if status == 429 else "enrollment_error")


def round_load(args, clients, pool):
    gate = Event()
    routes = ("snapshot", "spot") if args.mode == "mixed" else (args.mode,)

    def hit(client, route):
        gate.wait()
        # Separate openers keep concurrent requests independent for one device.
        connection = Client(client.target, context=args.context, timeout=client.timeout, token=client.token)
        return connection.measure(route, args.conditional)

    jobs = [(n, route, pool.submit(hit, client, route))
            for n, client in enumerate(clients) for route in routes]
    started = time.monotonic()
    gate.set()
    samples = {route: [] for route in routes}
    conditional, per_user = [], [[] for _ in clients]
    for n, route, future in jobs:
        sample, check = future.result()
        samples[route].append(sample)
        per_user[n].append(sample)
        if check is not None:
            conditional.append(check)
    first, full_core, core_available, spot_covered = [], [], 0, 0
    for observations in per_user:
        usable = [s for s in observations if s.ids & CORE]
        if usable:
            first.append((min(s.done for s in usable) - started) * 1000)
        union = set()
        for sample in sorted(observations, key=lambda s: s.done):
            union.update(sample.ids)
            if CORE <= union:
                full_core.append((sample.done - started) * 1000)
                core_available += 1
                break
        spot_covered += any(set(DEFAULT_IDS) <= s.ids for s in observations)
    elapsed = time.monotonic() - started
    result = {
        "elapsed_s": round(elapsed, 3),
        "http_requests_per_second": round(
            (len(jobs) + sum(s.result != "missing_etag" for s in conditional)) / elapsed, 1),
        "first_core_quote_ms": percentiles(first),
        "all_six_core_quotes_ms": percentiles(full_core),
        "users_with_any_core_quote": len(first),
        "users_with_all_core_quotes": core_available,
        "users_with_all_legacy_contract_quotes": spot_covered,
        "routes": {name: summarize(rows) for name, rows in samples.items()},
    }
    deadline = getattr(args, "core_deadline_ms", None)
    if deadline is not None:
        within = sum(latency <= deadline for latency in full_core)
        result["all_six_within_deadline"] = within
    if args.conditional:
        result["routes"]["conditional"] = summarize(conditional)
    return result


def warm_prices(args, client):
    """Warm the documented cache prerequisite without measuring it as an open."""
    started = time.monotonic()
    routes = ('snapshot', 'spot') if args.mode == 'mixed' else (args.mode,)
    required = set(routes)
    if args.mode == 'mixed' and not args.require_snapshot:
        required.discard('snapshot')
    samples = {route: [] for route in routes}
    passed = False
    while time.monotonic() - started < args.warmup_seconds:
        latest = {}
        for route in routes:
            sample, _ = client.measure(route)
            samples[route].append(sample)
            latest[route] = sample
        if all(latest[route].ok for route in required):
            passed = True
            break
        remaining = args.warmup_seconds - (time.monotonic() - started)
        if remaining > 0:
            time.sleep(min(1, remaining))
    return {'passed': passed, 'elapsed_s': round(time.monotonic() - started, 3),
            'routes': {route: summarize(rows) for route, rows in samples.items()}}


def enroll_clients(args, pool):
    """Retry only setup quota refusals, bounded to two additional windows."""
    clients, samples = [], []
    remaining = args.users
    windows = math.ceil(args.users / ENROLLMENT_BATCH) + ENROLLMENT_EXTRA_WINDOWS
    while remaining and windows:
        count = min(remaining, ENROLLMENT_BATCH)
        tasks = [pool.submit(enroll_user, args.base_url, args.context, args.timeout, getattr(args, 'enrollment_tunnel_port', None))
                 for _ in range(count)]
        retry = 0
        for future in tasks:
            client, sample = future.result()
            samples.append(sample)
            if client is not None:
                clients.append(client)
            elif sample.status == 429:
                retry += 1
        remaining -= count - retry
        windows -= 1
        if remaining and windows:
            print(f"Enrolled {len(clients)}/{args.users}; waiting for the enrollment window.",
                  file=sys.stderr, flush=True)
            time.sleep(ENROLLMENT_WINDOW_S)
    return clients, samples


def run(args):
    enrolled, enroll_samples, rounds, revoked = [], [], [], []
    sustained = []
    enrollment_started = time.monotonic()
    failed = False
    warmup = None
    try:
        with ThreadPoolExecutor(max_workers=min(args.users, args.enroll_workers)) as pool:
            enrolled, enroll_samples = enroll_clients(args, pool)
        enrollment_elapsed = time.monotonic() - enrollment_started
        failed = len(enrolled) != args.users
        if enrolled and args.warmup:
            warmup = warm_prices(args, enrolled[0])
            failed |= not warmup['passed']
            print(f"Warmup {'passed' if warmup['passed'] else 'failed'} in {warmup['elapsed_s']} s.", file=sys.stderr, flush=True)
        if enrolled:
            print(f"Starting {args.mode} load for {len(enrolled)} enrolled devices.",
                  file=sys.stderr, flush=True)
        if enrolled:
            with ThreadPoolExecutor(max_workers=len(enrolled) * (2 if args.mode == "mixed" else 1)) as pool:
                sustained_rounds = getattr(args, 'sustained_rounds', 0)
                for n in range(args.rounds + sustained_rounds):
                    is_sustained = n >= args.rounds
                    if n:
                        time.sleep(getattr(args, 'sustained_interval', 2) if is_sustained else args.interval)
                    row = round_load(args, enrolled, pool)
                    (sustained if is_sustained else rounds).append(row)
                    failed |= row["users_with_all_core_quotes"] != args.users
                    if args.mode != "snapshot":
                        failed |= row["users_with_all_legacy_contract_quotes"] != args.users
                    for name, route in row["routes"].items():
                        if name == "snapshot" and args.mode == "mixed" and not args.require_snapshot:
                            continue
                        count = route["outcome"].get("ok", 0)
                        if name == "conditional":
                            count += route["outcome"].get("changed", 0)
                            failed |= route["outcome"].get("ok", 0) == 0
                        failed |= count != args.users
                    if args.max_core_p95_ms is not None:
                        latency = row["all_six_core_quotes_ms"]
                        failed |= latency is None or latency["p95"] > args.max_core_p95_ms
    finally:
        if enrolled:
            with ThreadPoolExecutor(max_workers=min(len(enrolled), args.enroll_workers)) as pool:
                revoked = list(pool.map(lambda c: c.request("/v1/devices/revoke", method="POST")[0], enrolled))
    if args.core_deadline_ms is not None:
        achieved = sum(row["all_six_within_deadline"] for row in rounds)
        opportunities = args.users * args.rounds
        percent = 100 * achieved / opportunities
        failed |= percent < args.min_core_success_percent
    failed |= any(status != 204 for status in revoked)
    report = {
        "target": args.base_url,
        "mode": args.mode,
        "requested_users": args.users,
        "enrolled_users": len(enrolled),
        "enrollment_elapsed_s": round(enrollment_elapsed, 3),
        "enrollment": summarize(enroll_samples),
        "warmup": warmup,
        "rounds": rounds,
        "sustained_rounds": sustained,
        "revocation_status": dict(Counter(str(status) for status in revoked)),
        "passed": not failed,
    }
    if args.core_deadline_ms is not None:
        report["core_deadline_gate"] = {
            "deadline_ms": args.core_deadline_ms,
            "minimum_percent": args.min_core_success_percent,
            "opens_with_all_six_within_deadline": achieved,
            "opens_requested": opportunities,
            "percent": percent,
        }
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True, help="development-attestation node")
    parser.add_argument("--allow-remote", action="store_true", help="allow a non-loopback HTTPS target")
    parser.add_argument("--users", type=int, default=100)
    parser.add_argument("--mode", choices=("snapshot", "mixed", "spot"), default="mixed")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--interval", type=float, default=30, help="seconds between rounds after the previous one ends")
    parser.add_argument('--sustained-rounds', type=int, default=0,
                        help='additional qualified rounds before device revocation (0-20)')
    parser.add_argument('--sustained-interval', type=float, default=2,
                        help='seconds between sustained rounds (2-60)')
    parser.add_argument("--enroll-workers", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=15, help="HTTP socket timeout in seconds")
    parser.add_argument('--warmup', action='store_true',
                        help='warm required price caches after enrollment, before measured opens; report separately')
    parser.add_argument('--warmup-seconds', type=float, default=30,
                        help='retry budget for explicit warmup (default: 30 seconds)')
    parser.add_argument("--conditional", action="store_true", help="also check ETag revalidation; not the current app pattern")
    parser.add_argument("--require-snapshot", action="store_true",
                        help="in mixed mode, require full fresh snapshot coverage even if spot succeeds")
    parser.add_argument("--max-core-p95-ms", type=float,
                        help="fail if p95 time to all six prices in any round exceeds this limit")
    parser.add_argument("--core-deadline-ms", type=float,
                        help="measure fresh six-price coverage against a per-open deadline")
    parser.add_argument("--min-core-success-percent", type=float, default=99.9,
                        help="required percentage of opens within --core-deadline-ms")
    parser.add_argument("--ca-file", help="trusted CA bundle for a private development node")
    parser.add_argument('--enrollment-tunnel-port', type=int,
                        help='local SSH forward for setup only; measured prices remain direct HTTPS')
    args = parser.parse_args()
    try:
        args.base_url = base_url(args.base_url)
        if args.enrollment_tunnel_port is not None and (urlsplit(args.base_url).scheme != 'https' or not 1 <= args.enrollment_tunnel_port <= 65535):
            parser.error('enrollment tunnel requires HTTPS and a local port 1..65535')
        host = urlsplit(args.base_url).hostname
        try:
            loopback = ipaddress.ip_address(host).is_loopback
        except ValueError:
            loopback = host == "localhost"
        if not loopback and not args.allow_remote:
            parser.error("remote load requires --allow-remote")
        if (not 1 <= args.users <= 100 or not 1 <= args.rounds <= 20
                or not 0 <= args.sustained_rounds <= 20
                or not math.isfinite(args.sustained_interval) or not 2 <= args.sustained_interval <= 60
                or not 1 <= args.enroll_workers <= 16 or not math.isfinite(args.interval)
                or not 0 <= args.interval <= 3600 or not math.isfinite(args.timeout)
                or not 0 < args.timeout <= 60 or not 0 < args.warmup_seconds <= 120 or (args.conditional and args.mode == "spot")
                or (args.max_core_p95_ms is not None and
                    (not math.isfinite(args.max_core_p95_ms) or args.max_core_p95_ms <= 0))
                or (args.core_deadline_ms is not None and
                    (not math.isfinite(args.core_deadline_ms) or args.core_deadline_ms <= 0))
                or not 0 < args.min_core_success_percent <= 100):
            parser.error("out-of-range load settings or --conditional with --mode spot")
        args.context = ssl.create_default_context(cafile=args.ca_file)
    except (ProbeError, ValueError, OSError) as error:
        parser.error(str(error))
    report = run(args)
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
