#!/usr/bin/env python3
"""Builds tests/corpus/dashpay_upstream.json, the upstream the
DashPay route replays serve (tests/dashpay_route_test.py,
tests/dashpay_native_test.py), from two live captures
(tests/dashpay_capture.py) taken a few blocks apart.

Usage: python3 tests/dashpay_upstream_build.py OUT.json CAPTURE_A.json CAPTURE_B.json

  trusted   capture A's /quorums and /previous bytes as served, and its
            /masternodes reduced to real entries: three ineligible ones first,
            then three eligible ones (the native client must skip the former)
  replies   capture A's first evonode reply per label (raw gRPC-web bytes)
  newer     capture B's first evonode reply per label (a later block)
  clockMs   capture A's start: every reply's signed time is within the SDK's
            31-minute window of it
"""
import base64
import hashlib
import json
import sys


def main():
    out_path, a_path, b_path = sys.argv[1:4]
    a = json.load(open(a_path))
    b = json.load(open(b_path))
    nodes = json.loads(base64.b64decode(a["trusted"]["/masternodes"]))
    rows = nodes["data"]
    eligible = [r for r in rows if r.get("status") == "ENABLED" and r.get("versionCheck") == "success"]
    ineligible = [r for r in rows if not (r.get("status") == "ENABLED" and r.get("versionCheck") == "success")]
    kinds = []
    picked = []
    for row in ineligible:  # one per kind of ineligibility where available
        kind = (row.get("status"), row.get("versionCheck"))
        if kind not in kinds:
            kinds.append(kind)
            picked.append(row)
        if len(picked) == 3:
            break
    reduced = {"success": True, "data": picked + eligible[:3]}

    def first_replies(capture):
        replies = {}
        for reply in capture["replies"]:
            replies.setdefault(reply["label"], reply["body_b64"])
        return replies

    def port_of(row):
        return row.get("platformHTTPPort") or 443

    upstream = {
        "description": "Upstream for the DashPay route replays: live mainnet captures "
                       "(tests/dashpay_capture.py) built by tests/dashpay_upstream_build.py.",
        "captures": [{"captured_at_ms": c["captured_at_ms"], "nodes": c["nodes"]} for c in (a, b)],
        "clockMs": a["captured_at_ms"],
        "trusted": {
            "/quorums": base64.b64decode(a["trusted"]["/quorums"]).decode(),
            "/previous": base64.b64decode(a["trusted"]["/previous"]).decode(),
            "/masternodes": json.dumps(reduced, separators=(",", ":")),
        },
        "eligible": [f"{r['address'].rsplit(':', 1)[0]}:{port_of(r)}" for r in eligible[:3]],
        "replies": first_replies(a),
        "newer": first_replies(b),
    }
    data = (json.dumps(upstream, indent=1) + "\n").encode()
    with open(out_path, "wb") as handle:
        handle.write(data)
    with open(out_path + ".sha256", "w") as handle:
        handle.write(hashlib.sha256(data).hexdigest() + "\n")
    print(f"{out_path}: {len(upstream['replies'])} replies, {len(upstream['newer'])} newer, "
          f"{len(picked)} ineligible + 3 eligible nodes")


if __name__ == "__main__":
    main()
