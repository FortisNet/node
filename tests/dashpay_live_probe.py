#!/usr/bin/env python3
"""Opt-in live mainnet check of GET /v1/dashpay/names/resolve (never part of
make test: it needs egress to the Dash trusted quorum service and evonodes).

Enrolls a development device on a running native server (host or guest),
resolves real DPNS names as JSON and protobuf, and compares every answer
with the official Evo SDK (@dashevo/evo-sdk, trusted mainnet) resolving the
same names at the same time through the TypeScript resolver's own call
(`dpns.getUsernameByName`). Recorded identities are not an oracle: DPNS
domains are transferable, so the SDK is asked live.

Usage:
  dashpay_live_probe.py --base http://127.0.0.1:PORT
      [--sdk ../node_modules/@dashevo/evo-sdk/dist/evo-sdk.module.js]
      [name ...]
Exit status 1 when a name differs, 2 when a lookup was unavailable on both.
"""
import argparse
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SDK = ROOT / "node_modules/@dashevo/evo-sdk/dist/evo-sdk.module.js"
DEFAULT_NAMES = ["dash", "dashpay", "quantumexp10rer", "quantumexplorer", "zzzznotarealname1234", "a1ice"]

SDK_SCRIPT = r"""
const [moduleUrl, ...names] = process.argv.slice(1);
const { EvoSDK } = await import(moduleUrl);
const sdk = EvoSDK.mainnetTrusted({ settings: { connectTimeoutMs: 10000, timeoutMs: 12000, retries: 2,
  banFailedAddress: true } });
await sdk.connect();
const out = {};
for (const name of names) {
  try {
    const info = await sdk.dpns.getUsernameByName(name);
    out[name] = info === undefined ? null : info.identityId.toBase58();
  } catch (error) {
    out[name] = { error: String(error) };
  }
}
console.log(JSON.stringify(out));
process.exit(0);
"""


def call(base, method, path, headers=None, data=None, timeout=60):
    request = urllib.request.Request(base + path, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def enroll(base):
    status, _, body = call(base, "POST", "/v1/devices/challenge", data=b"")
    assert status == 200, (status, body)
    payload = json.dumps({"platform": "ios", "appVersion": "5.0.0", "attestation": {
        "provider": "development", "challenge": json.loads(body)["challenge"]}}).encode()
    status, _, body = call(base, "POST", "/v1/devices", {"Content-Type": "application/json"}, payload)
    assert status == 201, (status, body)
    return json.loads(body)["deviceToken"]


def protobuf_fields(payload):
    out, i = {}, 0
    while i < len(payload):
        key = payload[i]
        length = payload[i + 1]
        out[key >> 3] = payload[i + 2:i + 2 + length].decode()
        i += 2 + length
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True)
    parser.add_argument("--sdk", default=str(DEFAULT_SDK))
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    names = args.names or DEFAULT_NAMES
    token = enroll(args.base)
    auth = {"Authorization": "Bearer " + token}
    native = {}
    for name in names:
        started = time.monotonic()
        status, headers, body = call(args.base, "GET", f"/v1/dashpay/names/resolve?name={name}",
                                     {**auth, "Accept": "application/json"})
        elapsed = time.monotonic() - started
        pstatus, pheaders, pbody = call(args.base, "GET", f"/v1/dashpay/names/resolve?name={name}",
                                        {**auth, "Accept": "application/protobuf"})
        record = {"status": status, "seconds": round(elapsed, 2)}
        if status == 200:
            document = json.loads(body)
            record["identityId"] = document["identityId"]
            record["name"] = document["name"]
            fields = protobuf_fields(pbody) if pstatus == 200 else {}
            record["protobufAgrees"] = (pstatus == 200 and pheaders.get("Content-Type") == "application/protobuf"
                                        and fields.get(1) == document["name"]
                                        and fields.get(2) == document["identityId"])
        else:
            record["body"] = body.decode(errors="replace")
        native[name] = record
        print(f"native {name}: {record}")
    full = [f"{n}.dash" for n in names]
    completed = subprocess.run(["node", "--input-type=module", "-e", SDK_SCRIPT, Path(args.sdk).as_uri(), *full],
                               capture_output=True, text=True, timeout=300)
    if completed.returncode != 0:
        print("SDK run failed:", completed.stderr[-2000:])
        return 2
    sdk = json.loads(completed.stdout.strip().splitlines()[-1])
    differences = unavailable = 0
    for name, dotted in zip(names, full):
        theirs = sdk[dotted]
        mine = native[name]
        if isinstance(theirs, dict):
            print(f"SDK {dotted}: unavailable ({theirs['error'][:120]})")
            unavailable += mine["status"] != 200
            continue
        same = mine["status"] == 200 and mine["identityId"] == theirs and mine["name"] == dotted and mine["protobufAgrees"]
        print(f"{'MATCH' if same else 'DIFFERENT'} {dotted}: native {mine.get('identityId', mine.get('body'))} "
              f"sdk {theirs}")
        differences += not same
    if differences:
        return 1
    return 2 if unavailable else 0


if __name__ == "__main__":
    sys.exit(main())
