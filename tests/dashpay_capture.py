#!/usr/bin/env python3
"""Captures live Dash Platform evidence for the DashPay name resolver.

Records, at one moment, exactly what the native resolver reads:
  - the trusted quorum service's /quorums, /previous and /masternodes JSON
    (https://quorums.mainnet.networks.dash.org, the Evo SDK trusted context);
  - for each label, the raw gRPC-web getDocuments reply (V1 request, prove)
    from up to two eligible evonodes (publicly trusted IP certificates).

Usage: python3 tests/dashpay_capture.py OUT.json [label ...]

The output is the input of tests/dashpay_corpus_build.py. Standard library
only; TLS uses the platform's default verification (IP SANs included).
"""
import base64
import http.client
import json
import random
import ssl
import sys
import time

QUORUM_HOST = "quorums.mainnet.networks.dash.org"
DPNS = bytes.fromhex("e668c659af66aee1e72c186dde7b5b7e0a1d712a09c40d5721f622bf53c53155")
DEFAULT_LABELS = [
    "dash", "dashpay", "quantumexp10rer", "quantumexplorer", "alice", "a1ice",
    "zzzznotarealname1234", "a" * 63, "x-y", "000",
]


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def ld(num, payload):
    return varint(num << 3 | 2) + varint(len(payload)) + payload


def request_frame(label):
    def where(field, text):
        return ld(1, field.encode()) + ld(3, ld(5, text.encode()))
    v1 = (ld(1, DPNS) + ld(2, b"domain") + ld(3, where("normalizedLabel", label)) +
          ld(3, where("normalizedParentDomainName", "dash")) + varint(8 << 3) + varint(1) + ld(9, b""))
    message = ld(2, v1)
    return b"\x00" + len(message).to_bytes(4, "big") + message


def https(host, port, method, path, body=None, headers=None, timeout=15):
    context = ssl.create_default_context()
    connection = http.client.HTTPSConnection(host, port, timeout=timeout, context=context)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out_path = sys.argv[1]
    labels = sys.argv[2:] or DEFAULT_LABELS
    started = int(time.time() * 1000)
    trusted = {}
    for path in ("/quorums", "/previous", "/masternodes"):
        status, _, body = https(QUORUM_HOST, 443, "GET", path)
        if status != 200:
            raise SystemExit(f"{path}: HTTP {status}")
        trusted[path] = body
    nodes = json.loads(trusted["/masternodes"])["data"]
    eligible = [n for n in nodes if n.get("status") == "ENABLED" and n.get("versionCheck") == "success"]
    random.shuffle(eligible)
    replies = []
    used = []
    for node in eligible:
        if len(used) == 2:
            break
        host = node["address"].rsplit(":", 1)[0]
        port = node.get("platformHTTPPort") or 443
        node_replies = []
        try:
            for label in labels:
                status, headers, body = https(
                    host, port, "POST", "/org.dash.platform.dapi.v0.Platform/getDocuments",
                    body=request_frame(label),
                    headers={"content-type": "application/grpc-web+proto", "x-grpc-web": "1",
                             "accept": "application/grpc-web+proto"})
                if status != 200:
                    raise RuntimeError(f"HTTP {status}")
                node_replies.append({
                    "node": f"{host}:{port}", "label": label, "status": status,
                    "content_type": headers.get("content-type", ""),
                    "body_b64": base64.b64encode(body).decode(),
                })
        except (OSError, RuntimeError, ssl.SSLError, http.client.HTTPException) as error:
            print(f"skipping {host}: {error}", file=sys.stderr)
            continue
        used.append(f"{host}:{port}")
        replies.extend(node_replies)
    if not used:
        raise SystemExit("no evonode answered")
    capture = {
        "captured_at_ms": started,
        "finished_at_ms": int(time.time() * 1000),
        "quorum_host": QUORUM_HOST,
        "trusted": {path: base64.b64encode(body).decode() for path, body in trusted.items()},
        "nodes": used,
        "replies": replies,
    }
    with open(out_path, "w") as handle:
        json.dump(capture, handle, indent=1)
    print(f"{out_path}: {len(replies)} replies from {used}")


if __name__ == "__main__":
    main()
