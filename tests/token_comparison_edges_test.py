#!/usr/bin/env python3
"""R53: replay the frozen TypeScript POST /v1/tokens/compare edges
(tests/corpus/token_comparison_edges.json, written by
the earlier TypeScript implementation's tests and checked
against its SHA-256 pin) against build/fortis_node_discovery_route_test
over real sockets, then run the native-only concurrency check.

Replay. Each fixture group runs in a fresh process over one KV directory;
the previous process is SIGKILLed first (a hard restart: only what reached
disk survives). A group's seed rows (the production projections' patches)
are loaded at boot through FORTIS_TEST_COMPARISON_SEED
(tests/token_comparison_seed.h), as TypeScript patches them right after its
boot; the first group also plants the undecodable document that stands for
TypeScript's injected store read failure. Every request runs at its fixture
clock (FORTIS_TEST_DISCOVERY_NOW_FILE); status, every fixture header
(x-rate-limit-remaining included: each device letter is used in one group
and one fixture minute, and each group starts with at least 20 s of the wall
minute left) and the exact body bytes (JSON text or protobuf wire) must
match. `concurrent` cases send their copies at once; every answer must equal
the recorded one and the remaining values must be the recorded multiset.

Asserted native differences (each checked, never skipped):
  jsonLoneSurrogate  the shared bounded JSON reader refuses a lone
                     surrogate escape: 400 {"error":"BAD_REQUEST"} after the
                     device policy, cost 1 (fastify-errors;
                     TypeScript reads it and echoes it as INVALID_TOKEN).

Native-only check (no TypeScript equivalent):
  concurrency  after a restart, 16 clients send 40 requests each, mixing
               eight request kinds (JSON and protobuf answers, a 304, a zod
               refusal, invalid refs, a store failure, a form body, an
               unknown bearer); every answer must equal the kind's answer
               taken alone at the same clock (status and body bytes, and
               every header but x-rate-limit-remaining).

Usage: token_comparison_edges_test.py <discovery route test binary>
Environment: COMPARISON_EDGES_ONLY_GROUPS=main,quota limits the replay;
COMPARISON_EDGES_SKIP_NATIVE=1 skips the native-only check;
COMPARISON_EDGES_FAIL_FAST=1 stops at the first failing group.
"""
import hashlib
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

import compat_expectations

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE = "token_comparison_edges.json"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after"]
NATIVE_INVALID_JSON = '{"error":"BAD_REQUEST"}'
# auth-first (fastify-errors): the device policy refuses these before
# the body is judged (the quota is spent, or attestation is required).
POLICY_FIRST = {"quotaOverForm": (429, b'{"error":"RATE_LIMITED"}'),
                "quotaOverInvalidJson": (429, b'{"error":"RATE_LIMITED"}'),
                "quotaOverMediaType": (429, b'{"error":"RATE_LIMITED"}'),
                "attestationInvalidJsonFirst": (401, b'{"error":"ATTESTATION_REQUIRED"}')}
DIFFERENCES = {"jsonLoneSurrogate"}

failures = []


def fail(message):
    failures.append(message)
    print("FAIL:", message, file=sys.stderr)


def load_fixture():
    raw = (FIXTURES / FIXTURE).read_bytes()
    pinned = (FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(raw).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(raw)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def raw_request(port, headers, body, timeout=30):
    """The exact bytes the TypeScript oracle wrote: Host, Connection: close,
    the recorded headers in order, Content-Length, the body."""
    lines = ["POST /v1/tokens/compare HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers.items()]
    lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + body)
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    head_lines = head.decode("latin1").split("\r\n")
    status = int(head_lines[0].split()[1])
    parsed = {}
    for line in head_lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            parsed[key.strip().lower()] = value.strip()
    return status, parsed, payload


def expected_payload(case):
    return bytes.fromhex(case["wireHex"]) if "wireHex" in case else case["bodyText"].encode("utf-8")


QUOTA_SHIFT = compat_expectations.QuotaShift()
COMPARISON_REFUSAL = b'{"error":"BAD_REQUEST","detail":"invalid comparison request"}'


def strict_case(case, answer_headers):
    """The case as this server answers it (compat-decisions.md
    #form-bodies, #js-prototype-keys, #zod-issues); None when unchanged."""
    request_headers, body = request_parts(case)
    if case["name"] == "protobufFiatBom":
        payload = b'{"error":"BAD_REQUEST","detail":"invalid comparison request"}'
        headers = dict(case["headers"], **{"content-length": str(len(payload)), "cache-control": "no-store"})
        headers.pop("etag", None)
        return dict(case, status=400, headers=headers, bodyText=payload.decode()), True
    if "wireHex" in case:
        return None
    status, headers, text, known = compat_expectations.remap(
        case["status"], case["headers"], case["bodyText"], request_headers.get("content-type"), body,
        request_headers.get("authorization"), QUOTA_SHIFT, COMPARISON_REFUSAL, request_headers=request_headers)
    # The __proto__ case: charged by this server, after the quota.
    headers = compat_expectations.settle_charged(headers, answer_headers, request_headers.get("authorization"), QUOTA_SHIFT)
    return dict(case, status=status, headers=headers, bodyText=text.decode("utf-8", "surrogatepass")), known


def compare(case, status, headers, payload, remaining=True):
    if case["name"] in POLICY_FIRST:
        want_status, want_body = POLICY_FIRST[case["name"]]
        if status != want_status or payload != want_body:
            fail(f"{case['name']}: the device policy does not answer first: {status} {payload[:120]!r}")
            return False
        return True
    mapped = strict_case(case, headers) if remaining else None
    if mapped:
        case, remaining = mapped
    problems = []
    if status != case["status"]:
        problems.append(f"status {status} != {case['status']} ({payload[:200]!r})")
    for header in HEADERS:
        if header == "x-rate-limit-remaining" and not remaining:
            continue
        want = case["headers"].get(header)
        got = headers.get(header)
        if want != got:
            problems.append(f"{header}: {got!r} != {want!r}")
    want = expected_payload(case)
    if payload != want:
        at = next((i for i, (a, b) in enumerate(zip(payload, want)) if a != b), min(len(payload), len(want)))
        problems.append(f"body differs at byte {at} (got {len(payload)}, want {len(want)}): "
                        f"{payload[max(0, at - 60):at + 60]!r} != {want[max(0, at - 60):at + 60]!r}")
    if problems:
        fail(f"{case['name']}: " + "; ".join(problems))
        return False
    return True


def assert_difference(case, status, headers, payload):
    """The explicit Unicode replacement has one body form and a new ETag."""
    expected = case["bodyText"].replace("\\ud800", "\ufffd").encode()
    validator = json.loads(expected)
    validator.pop("computedAt")
    etag = 'W/"token-comparison-' + hashlib.sha256(json.dumps(validator, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()[:24] + '"'
    ok = (status == 200 and payload == expected and headers.get("etag") == etag
          and headers.get("x-rate-limit-cost") == "1")
    if not ok:
        fail(f"{case['name']}: Unicode replacement body/ETag differs: {status} {headers} {payload[:200]!r}")
    return ok


def unicode_conditional(server, case, etag):
    headers, body = request_parts(case)
    for validator, expected in ((case["headers"]["etag"], 200), (etag, 304)):
        status, response, payload = raw_request(server.port, dict(headers, **{"If-None-Match": validator}), body)
        if status != expected or response.get("etag") != etag or (status == 304 and payload):
            fail(f"Unicode conditional {validator}: {status} {response} {payload[:120]!r}")
            return False
    QUOTA_SHIFT.charged(headers.get("authorization"), 2)
    return True


def seed_lines(rows):
    lines = []
    for row in rows:
        for field, value in row["fields"].items():
            if value is None:
                kind, text = "null", "-"
            elif isinstance(value, dict):
                kind, text = "date", str(value["ms"])
            elif isinstance(value, str):
                kind, text = "str", value.encode("utf-8").hex()
            elif isinstance(value, bool):
                raise ValueError("boolean seed values are not supported")
            else:
                kind, text = "num", json.dumps(value)
            lines.append("\t".join([row["bucket"], row["network"], row["tokenId"], field, kind, text]))
    return lines


class Server:
    def __init__(self, binary, root):
        self.binary = binary
        self.root = Path(root)
        self.kv = self.root / "kv"
        self.kv.mkdir(parents=True, exist_ok=True)
        self.now_file = self.root / "now"
        self.process = None
        self.port = None
        self.boots = 0

    def set_clock(self, ms):
        temporary = self.root / "now.tmp"
        temporary.write_text(str(ms))
        os.replace(temporary, self.now_file)

    def start(self, at, seed_rows, env_extra, first):
        self.crash()
        self.boots += 1
        seed = self.root / f"seed-{self.boots}.tsv"
        lines = seed_lines(seed_rows)
        if first:
            lines.append("\t".join(["comparison", FIXTURE_DATA["storeFailureNetwork"], "native", "-", "corrupt", "-"]))
        seed.write_text("".join(line + "\n" for line in lines))
        self.set_clock(at)
        self.port = free_port()
        env = {key: value for key, value in os.environ.items() if not key.startswith("FORTIS_")}
        env.pop("ATTESTATION_MODE", None)
        env.update({"FORTIS_ENABLE_TEST_DISCOVERY_ROUTES": "1", "FORTIS_KV_DIR": str(self.kv),
                    "PORT": str(self.port), "FORTIS_TEST_COMPARISON_SEED": str(seed) if lines else "",
                    "FORTIS_TEST_DISCOVERY_NOW_FILE": str(self.now_file),
                    "FORTIS_TEST_DISCOVERY_NOW_MS": str(at)})
        env.update(env_extra)
        self.process = subprocess.Popen([self.binary, str(self.port)], env=env, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.PIPE)
        for _ in range(400):
            if self.process.poll() is not None:
                raise RuntimeError(self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.025)
        raise RuntimeError("discovery test server did not start")

    def crash(self):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL)
            self.process.wait()
        self.process = None


def wait_for_fresh_minute(budget_seconds):
    left = 60 - time.time() % 60
    if left < budget_seconds:
        time.sleep(left + .2)


def request_parts(case):
    return case["requestHeaders"], bytes.fromhex(case.get("requestHex", ""))


def run_case(server, case, tally):
    server.set_clock(case["at"])
    headers, body = request_parts(case)
    count = case.get("concurrent")
    if count:
        answers = [None] * count

        def one(index):
            answers[index] = raw_request(server.port, headers, body)

        threads = [threading.Thread(target=one, args=(i,)) for i in range(count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        ok = all(compare(case, *answer, remaining=False) for answer in answers)
        got = sorted(answer[1].get("x-rate-limit-remaining") or "" for answer in answers)
        want = sorted(value or "" for value in case["responses"])
        if got != want:
            fail(f"{case['name']}: concurrent remaining {got} != {want}")
            ok = False
        tally["matched" if ok else "failed"] += 1
        return ok
    status, response, payload = raw_request(server.port, headers, body)
    if case["name"] in DIFFERENCES:
        ok = assert_difference(case, status, response, payload)
        if ok: ok = unicode_conditional(server, case, response["etag"])
        tally["asserted" if ok else "failed"] += 1
        return ok
    ok = compare(case, status, response, payload)
    tally["matched" if ok else "failed"] += 1
    return ok


def replay(binary, root, fixture):
    only = {name for name in os.environ.get("COMPARISON_EDGES_ONLY_GROUPS", "").split(",") if name}
    fail_fast = os.environ.get("COMPARISON_EDGES_FAIL_FAST") == "1"
    server = Server(binary, root)
    tally = {"matched": 0, "asserted": 0, "failed": 0}
    by_group = {}
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    try:
        for index, group in enumerate(fixture["groups"]):
            name = group["name"]
            cases = by_group.get(name, [])
            # Seeds and restarts run for every group: a skipped group's rows
            # are still on disk for the groups after it, as in TypeScript.
            server.start(group["start"], group["seed"], group["env"], index == 0)
            if only and name not in only:
                continue
            wait_for_fresh_minute(20)
            QUOTA_SHIFT.reset()
            before = tally["failed"]
            for case in cases:
                run_case(server, case, tally)
            print(f"group {name}: {len(cases)} cases, {tally['failed'] - before} failed")
            if fail_fast and tally["failed"] > before:
                break
    finally:
        server.crash()
    return tally


# ---- native-only: concurrency ----------------------------------------------------------------------------

KINDS = [
    ("json", {"accept": "application/json", "content-type": "application/json"},
     json.dumps({"tokens": [{"network": "eth-mainnet", "tokenId": "erc20:0x" + "11" * 20},
                            {"network": "bnb", "tokenId": "native"}]}).encode()),
    ("protobufAnswer", {"accept": "application/protobuf", "content-type": "application/json"},
     json.dumps({"tokens": [{"network": "eth-mainnet", "tokenId": "0x" + "44" * 20},
                            {"network": "base", "tokenId": "native"}]}).encode()),
    ("protobufRequest", {"accept": "application/protobuf", "content-type": "application/protobuf"},
     bytes.fromhex("0a0d0a03626e621206" + b"native".hex() + "1203757364")),
    ("zodRefusal", {"accept": "application/json", "content-type": "application/json"}, b'{"tokens":[]}'),
    ("invalidRefs", {"accept": "application/json", "content-type": "application/json"},
     json.dumps({"tokens": [{"network": "nowhere", "tokenId": "native"},
                            {"network": "bnb", "tokenId": "native"}, {"network": "eip155:56", "tokenId": "native"}]}).encode()),
    ("storeFailure", {"accept": "application/json", "content-type": "application/json"},
     json.dumps({"tokens": [{"network": "cronos", "tokenId": "native"}]}).encode()),
    ("form", {"accept": "application/json", "content-type": "application/x-www-form-urlencoded"}, b"tokens=bnb"),
    ("unknownBearer", {"accept": "application/json", "content-type": "application/json"}, b'{"tokens":[]}'),
]


def native_concurrency(binary, root, fixture):
    server = Server(binary, root)
    letters = "ABCDEFGIJKLMNOPQRSTUVWXYZ"
    at = fixture["now"] + 20 * 1000
    try:
        server.start(at, fixture["groups"][0]["seed"], {}, True)
        wait_for_fresh_minute(25)

        def headers_for(kind, letter):
            headers = {"accept": kind[1]["accept"]}
            token = "0" if kind[0] == "unknownBearer" else letter
            headers["authorization"] = "Bearer fintel_dev_" + token * 43
            headers["content-type"] = kind[1]["content-type"]
            return headers

        expected = {}
        for kind in KINDS:
            expected[kind[0]] = raw_request(server.port, headers_for(kind, "H"), kind[2])
        etag = expected["json"][1]["etag"]
        revalidate = ("revalidate", {"accept": "application/json", "content-type": "application/json"}, KINDS[0][2])
        reval_headers = dict(headers_for(revalidate, "H"), **{"if-none-match": etag})
        expected["revalidate"] = raw_request(server.port, reval_headers, KINDS[0][2])
        if expected["revalidate"][0] != 304 or expected["json"][0] != 200 or expected["storeFailure"][0] != 500:
            fail(f"concurrency: unexpected baselines {[(k, v[0]) for k, v in expected.items()]}")
        kinds = KINDS + [revalidate]
        errors = []
        counts = {"requests": 0}
        lock = threading.Lock()

        def client(index):
            letter = letters[index % len(letters)]
            for step in range(40):
                kind = kinds[(index + step) % len(kinds)]
                headers = headers_for(kind, letter)
                if kind[0] == "revalidate":
                    headers["if-none-match"] = etag
                status, response, payload = raw_request(server.port, headers, kind[2])
                want = expected[kind[0]]
                same = status == want[0] and payload == want[2] and all(
                    response.get(h) == want[1].get(h) for h in HEADERS if h != "x-rate-limit-remaining")
                with lock:
                    counts["requests"] += 1
                    if not same:
                        errors.append(f"{kind[0]} from client {index}: {status} {payload[:120]!r}")

        started = time.time()
        threads = [threading.Thread(target=client, args=(i,)) for i in range(16)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        elapsed = time.time() - started
        for error in errors[:10]:
            fail(f"concurrency: {error}")
        print(f"native concurrency: {counts['requests']} requests from 16 clients over {len(kinds)} kinds, "
              f"{len(errors)} wrong, {elapsed:.2f} s")
    finally:
        server.crash()


FIXTURE_DATA = {}


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    fixture = load_fixture()
    FIXTURE_DATA.update(fixture)
    with tempfile.TemporaryDirectory(prefix="fortis-compare-edges-") as root:
        tally = replay(binary, Path(root) / "replay", fixture)
        print(f"token comparison edges: {tally['matched']} matched, {tally['asserted']} asserted differences, "
              f"{tally['failed']} failed")
        if os.environ.get("COMPARISON_EDGES_SKIP_NATIVE") != "1":
            native_concurrency(binary, Path(root) / "native", fixture)
    if failures:
        print(f"token comparison edges: {len(failures)} failures", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
