#!/usr/bin/env python3
"""Byte-for-byte replay of the frozen TypeScript POST /v1/tokens/compare
contract (tests/corpus/token_comparison_route.json) against the
native route.

Usage: token_comparison_route_test.py <binary>

The binary is the discovery route test build of main.cpp
(-DFORTIS_DISCOVERY_ROUTE_TEST_ONLY). Contract: argv[1] is the port (PORT is
also set), FORTIS_ENABLE_TEST_DISCOVERY_ROUTES=1 registers the route,
FORTIS_KV_DIR is a fresh directory, FORTIS_TEST_COMPARISON_SEED names the
seed file (tests/token_comparison_seed.h), FORTIS_TEST_DISCOVERY_NOW_FILE
holds the route clock in epoch ms (re-read per request) and
FORTIS_TEST_DISCOVERY_NOW_MS its initial value, and the device token
fintel_dev_HHH... is seeded as in the other route test binaries.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

import compat_expectations

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/token_comparison_route.json"
TOKEN = "fintel_dev_" + "H" * 43
HEADER_NAMES = ["content-type", "content-length", "vary", "cache-control", "etag",
                "x-content-type-options", "referrer-policy", "permissions-policy",
                "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after"]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def post(port, spec):
    body = b""
    if "hex" in spec:
        body = bytes.fromhex(spec["hex"])
    elif "text" in spec:
        body = spec["text"].encode("utf-8")
    lines = ["POST /v1/tokens/compare HTTP/1.1", "Host: localhost", "Connection: close",
             "Accept: " + spec["accept"]]
    if spec.get("authorization", True) is not None:
        lines.append("Authorization: Bearer " + TOKEN)
    if spec.get("origin"):
        lines.append("Origin: " + spec["origin"])
    if spec.get("ifNoneMatch"):
        lines.append("If-None-Match: " + spec["ifNoneMatch"])
    if spec.get("contentType"):
        lines.append("Content-Type: " + spec["contentType"])
    if spec.get("contentType") or body:
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    header, _, payload = bytes(data).partition(b"\r\n\r\n")
    head = header.decode("latin1").split("\r\n")
    status = int(head[0].split()[1])
    headers = {}
    for line in head[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    return status, headers, payload


def seed_lines(fixture):
    lines = []
    for row in fixture["seed"]:
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
            lines.append("\t".join([row["bucket"], row["network"], row["tokenId"], field, kind,
                                    text]))
    # TypeScript's tokenStore.get throws for this network: an undecodable
    # native document is the equivalent store read failure.
    network = fixture["source"]["storeFailureNetwork"]
    lines.append("\t".join(["comparison", network, "native", "-", "corrupt", "-"]))
    return "\n".join(lines) + "\n"


def start(binary, directory, fixture):
    kv = directory / "kv"
    kv.mkdir(parents=True)
    seed = directory / "seed.tsv"
    seed.write_text(seed_lines(fixture))
    now_file = directory / "now"
    first = fixture["cases"][fixture["order"][0]]["nowMs"]
    now_file.write_text(str(first))
    port = free_port()
    env = os.environ.copy()
    env.update({
        "FORTIS_ENABLE_TEST_DISCOVERY_ROUTES": "1",
        "FORTIS_KV_DIR": str(kv),
        "PORT": str(port),
        "FORTIS_TEST_COMPARISON_SEED": str(seed),
        "FORTIS_TEST_DISCOVERY_NOW_FILE": str(now_file),
        "FORTIS_TEST_DISCOVERY_NOW_MS": str(first),
    })
    process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return process, port, now_file
        except OSError:
            time.sleep(.03)
    process.kill()
    raise RuntimeError("discovery test server did not start")


COMPARISON_REFUSAL = b'{"error":"BAD_REQUEST","detail":"invalid comparison request"}'


def strict_case(case, answer_headers, shift):
    """The case as this server answers it (compat-decisions.md
    #form-bodies, #js-prototype-keys, #zod-issues)."""
    if "wireHex" in case["body"]:
        return case
    spec = case["request"]
    body = bytes.fromhex(spec["hex"]) if "hex" in spec else spec.get("text", "").encode("utf-8")
    status, headers, text, _ = compat_expectations.remap(
        case["status"], case["headers"], case["body"]["text"], spec.get("contentType"), body, "device",
        shift, COMPARISON_REFUSAL)
    # The __proto__ case: charged by this server, after the quota.
    headers = compat_expectations.settle_charged(headers, answer_headers, "device", shift)
    return dict(case, status=status, headers=headers, body=dict(case["body"], text=text.decode("utf-8")))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    raw = FIXTURE.read_bytes()
    pinned = FIXTURE.with_name(FIXTURE.name + ".sha256").read_text().strip()
    if hashlib.sha256(raw).hexdigest() != pinned:
        print("fixture hash mismatch", file=sys.stderr)
        return 1
    fixture = json.loads(raw)
    for name in ('protobufFiatBom','protobufWrongWireTypeString','protobufNestedOverrun'):
        case = fixture['cases'][name]
        case['status'] = 400
        case['body'] = {'text': COMPARISON_REFUSAL.decode()}
        case['headers'] = {key:value for key,value in case['headers'].items() if key not in ('etag','vary')}
        case['headers'].update({'content-length':str(len(COMPARISON_REFUSAL)), 'cache-control':'no-store'})
    # Quota counters are per wall-clock minute: keep the replay inside one.
    left = 60 - time.time() % 60
    if left < 20:
        time.sleep(left + .2)
    failures = []
    shift = compat_expectations.QuotaShift()
    with tempfile.TemporaryDirectory(prefix="fortis-compare-") as root:
        process, port, now_file = start(binary, Path(root), fixture)
        try:
            for name in fixture["order"]:
                case = fixture["cases"][name]
                now_file.write_text(str(case["nowMs"]))
                status, headers, payload = post(port, case["request"])
                case = strict_case(case, headers, shift)
                problems = []
                if status != case["status"]:
                    problems.append(f"status {status} != {case['status']}")
                for header in HEADER_NAMES:
                    want = case["headers"].get(header)
                    got = headers.get(header)
                    if want != got:
                        problems.append(f"{header}: {got!r} != {want!r}")
                body = case["body"]
                if "wireHex" in body:
                    if payload.hex() != body["wireHex"]:
                        problems.append(f"protobuf {payload.hex()[:200]} != {body['wireHex'][:200]}")
                elif payload.decode("utf-8", "replace") != body["text"]:
                    want = body["text"]
                    got = payload.decode("utf-8", "replace")
                    at = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b),
                              min(len(got), len(want)))
                    problems.append(f"body differs at {at}: {got[max(0, at - 80):at + 80]!r} != "
                                    f"{want[max(0, at - 80):at + 80]!r}")
                if problems:
                    failures.append((name, problems))
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
    for name, problems in failures:
        print(f"FAIL {name}:", file=sys.stderr)
        for problem in problems:
            print("   ", problem, file=sys.stderr)
    replayed = len(fixture["order"])
    if failures:
        print(f"token comparison route: {len(failures)} of {replayed} cases differ", file=sys.stderr)
        return 1
    print(f"token comparison route: {replayed} TypeScript cases replayed byte-for-byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
