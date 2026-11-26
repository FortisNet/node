#!/usr/bin/env python3
"""Signed AI request helpers and durable open-route checks.

The earlier synchronous-reply/expiring-thread assertions are replaced by
current-contract tests below. Fixtures remain immutable.
"""
import base64
import hashlib
import json
import socket
import sys
from datetime import datetime, timedelta, timezone

import ai_conversations_route_test as replay
import discovery_route_harness as harness
import evm_sign

PATH = "/v1/ai/conversations"
NAMES = ["address", "nonce", "timestamp", "snapshotHash", "accessToken", "accessTokenHash", "snapshotJson",
         "signature", "firstMessage", "clientMessageId"]
SIGNER = evm_sign.Signer(int("12" * 32, 16))
TOKENS = {"A": "fintel_dev_" + "A" * 43, "B": "fintel_dev_" + "B" * 43}
DEVICES = {}
HOUR = 3600 * 1000
JSON_TYPE = "application/json; charset=utf-8"

# ---- requests ----------------------------------------------------------------------

def raw(server, head_lines, body=b"", timeout=60):
    data = bytearray()
    try:
        with socket.create_connection(("127.0.0.1", server.port), timeout=timeout) as sock:
            # The server may answer and close before reading the whole body
            # (413 on Content-Length): keep reading what it sent.
            try:
                sock.sendall(("\r\n".join(head_lines) + "\r\n\r\n").encode("latin1") + body)
            except OSError:
                pass
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data.extend(chunk)
    except OSError:
        pass
    if not data.startswith(b"HTTP/1.1 "):
        return None
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    first, *lines = head.decode("latin1").split("\r\n")
    headers = {}
    for line in lines:
        key, _, value = line.partition(":")
        headers[key.lower()] = value.strip()
    status = int(first.split()[1])
    return status, headers, payload


def encode_json(payload):
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8", "surrogatepass")


def call(server, payload=None, *, body=None, content_type="application/json", device="A", accept="application/json",
         extra=(), path=PATH, method="POST"):
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    if accept is not None:
        lines.append(f"Accept: {accept}")
    if device is not None:
        lines.append(f"Authorization: Bearer {TOKENS.get(device, device)}")
    if body is None and payload is not None:
        body = encode_json(payload)
    if body is not None and content_type is not None:
        lines.append(f"Content-Type: {content_type}")
    lines.extend(extra)
    body = body or b""
    lines.append(f"Content-Length: {len(body)}")
    return raw(server, lines, body)


def history(server, conversation, token, device="A"):
    result = raw(server, [f"GET {PATH}/{conversation}/messages HTTP/1.1", "Host: localhost", "Connection: close",
                          "Accept: application/json", f"Authorization: Bearer {TOKENS[device]}",
                          f"X-Intelligence-Authorization: Bearer {token}"])
    if not result or result[0] != 200:
        return None
    return [(m["id"], m["role"], m["body"], m["createdAt"]) for m in json.loads(result[2])["messages"]]



# ---- proofs, bodies and wires --------------------------------------------------------

def sha(text):
    return hashlib.sha256(text.encode("utf-8", "surrogatepass")).hexdigest()


def iso(ms):
    moment = datetime(1970, 1, 1, tzinfo=timezone.utc) + timedelta(milliseconds=ms)
    return moment.strftime("%Y-%m-%dT%H:%M:%S.") + f"{ms % 1000:03d}Z"


def ulid(n):
    return f"01JZ{n:022d}"


def chat_token(n):
    return "fintel_chat_" + base64.urlsafe_b64encode(hashlib.sha256(f"chat-{n}".encode()).digest()).decode().rstrip("=")


def snapshot_json(captured, address=None, pad=0):
    text = json.dumps({
        "version": 1, "capturedAt": iso(captured), "balancesComplete": False,
        "wallets": [{"id": "hd:evm:0", "address": address or SIGNER.address, "family": "evm",
                     "networkIds": ["eth-mainnet"], "watchOnly": False},
                    {"id": "watch:solana:1", "address": "SolanaPrivateMarker1111111111111111111111111",
                     "family": "solana", "networkIds": ["solana-mainnet"], "watchOnly": True}],
        "tokens": [{"assetId": "eth-mainnet:native", "networkId": "eth-mainnet", "symbol": "ETH", "decimals": 18,
                    "source": "native"}],
        "holdings": [{"walletId": "hd:evm:0", "assetId": "eth-mainnet:native", "displayUnits": "7.4321"}],
    }, separators=(",", ":"))
    return text + " " * pad


def make_open(at, n, *, device="A", snapshot=None, token=None, nonce=None, client=None, message="What do I hold?",
              timestamp=None, signer=SIGNER, address=None, signed_address=None):
    snapshot = snapshot if snapshot is not None else snapshot_json(at - 60_000)
    token = token or chat_token(n)
    nonce = nonce or sha(f"nonce-{n}")[:32]
    timestamp = timestamp or iso(at)
    address = address or SIGNER.address
    proof = "\n".join([
        "FortisNet Intelligence conversation v1", f"Address: {signed_address or address}",
        f"Device: {DEVICES[device]}", f"Nonce: {nonce}", f"Timestamp: {timestamp}",
        f"Snapshot SHA-256: {sha(snapshot)}", f"Session token SHA-256: {sha(token)}"])
    request = {"address": address, "nonce": nonce, "timestamp": timestamp, "snapshotHash": sha(snapshot),
               "accessToken": token, "accessTokenHash": sha(token), "snapshotJson": snapshot,
               "signature": signer.sign_message(proof), "firstMessage": message,
               "clientMessageId": client or sha(f"client-{n}")[:32]}
    return request


def varint(value):
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def proto_open(request):
    out = bytearray()
    for number, name in enumerate(NAMES, 1):
        value = request[name].encode("utf-8")
        if value:
            out += bytes([number << 3 | 2]) + varint(len(value)) + value
    return bytes(out)


def wire_fields(wire):
    fields, at = {}, 0
    while at < len(wire):
        tag, shift = 0, 0
        while True:
            byte = wire[at]
            at += 1
            tag |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        size, shift = 0, 0
        while True:
            byte = wire[at]
            at += 1
            size |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        fields[tag >> 3] = wire[at:at + size]
        at += size
    return fields


def main(binary):
    DEVICES.update(replay.load_fixture(replay.FIXTURE)["devices"])
    from ai_current_contract import running, expect as require_response, opened
    with running(binary) as (server, root, upstream, now):
        for body in (b"null", b"[]", b"{}", b'{"address":42}'):
            require_response(call(server, body=body), 400, "BAD_REQUEST")
        request, first = opened(server, now)
        # Exact proof replay saves no additional user turn, including protobuf.
        require_response(call(server, request), 201)
        assert len(history(server, first['id'], request['accessToken'])) == 1
        _, payload = require_response(call(server, body=proto_open(request), content_type="application/protobuf",
                                          accept="application/protobuf"), 201)
        fields = wire_fields(payload)
        assert set(fields) == {1, 2, 4}, fields
        assert fields[1].decode() == first['id'] and fields[2].decode() == request['accessToken']
        assert wire_fields(fields[4])[3].decode() == request['firstMessage']
        # ASCII case folding preserves the owner HMAC: a freshly signed
        # lowercase address recovers the same durable thread, rotating token.
        for label, address in [('lowercase', SIGNER.address.lower()),
                               ('uppercase', '0x' + SIGNER.address[2:].upper())]:
            lower = make_open(now, label, address=address)
            _, payload = require_response(call(server, lower), 201)
            recovered = json.loads(payload)
            assert recovered['id'] == first['id'], 'every accepted EVM spelling keeps the owner HMAC'
        assert history(server, first['id'], request['accessToken']) is None
        assert len(history(server, first['id'], lower['accessToken'])) == 3
        # __proto__ is an ordinary unknown member; no prototype poisoning.
        ordinary = make_open(now, "prototype")
        ordinary['__proto__'] = {"privateMarker": "ignored"}
        require_response(call(server, ordinary), 400, "BAD_REQUEST")
        # Every signed address must still pass the EVM grammar/proof rules.
        for address in ("0x" + "f" * 39 + "é", "Solana111111111111111111111111111111111111"):
            bad = make_open(now, address, address=address)
            require_response(call(server, bad), 400, "BAD_REQUEST")
        # Authenticated schema failures and proof refusals expose no proof data.
        bad = dict(lower, signature="0x" + "00" * 65)
        require_response(call(server, bad), 401, "PROOF_INVALID")
        logs = (root / 'stderr.log').read_bytes() + (root / 'stdout.log').read_bytes()
        for secret in (request['accessToken'], request['nonce'], request['signature'], request['snapshotJson']):
            assert secret.encode() not in logs
    # Hourly open quota: exact repeats count; body schema refusals do not.
    with running(binary) as (server, root, upstream, now):
        request = make_open(now, "quota")
        for _ in range(20):
            require_response(call(server, request), 201)
        headers, _ = require_response(call(server, request), 429, "RATE_LIMITED")
        assert headers['retry-after'] == '3600'
        require_response(call(server, body=b'{}'), 400, 'BAD_REQUEST')
        harness.set_clock(root / 'clock', now + HOUR)
        refreshed = make_open(now + HOUR, "quota-next")
        require_response(call(server, refreshed), 201)
    print('AI open: proof, owner case, JSON/protobuf presence, replay, privacy and quota passed')


if __name__ == "__main__":
    main(sys.argv[1])
