#!/usr/bin/env python3
"""AI production gating and shared signed-request helpers.

Synchronous OpenAI slot/deadline assertions were superseded by the durable
thread and asynchronous Anthropic suites.
"""
import base64
import hashlib
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

import discovery_route_harness as harness
import evm_sign
import tls_ca

SECRET = "native-test-support-secret-0123456789abcdef"
DEVICE_A = "01ARZ3NDEKTSV4RRFFQ69G5FAV"
TOKEN_A = "fintel_dev_" + "A" * 43
NOW = 1_772_323_200_000  # 2026-03-01T00:00:00Z
SIGNER = evm_sign.Signer(int("5a" * 32, 16))


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def iso(ms):
    return datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.") + f"{ms % 1000:03d}Z"


def snapshot_json():
    return json.dumps({
        "version": 1, "capturedAt": iso(NOW - 60_000), "balancesComplete": True,
        "wallets": [{"id": "hd:evm:0", "address": SIGNER.address, "family": "evm",
                     "networkIds": ["eth-mainnet"], "watchOnly": False}],
        "tokens": [{"assetId": "eth-mainnet:native", "networkId": "eth-mainnet", "symbol": "ETH",
                    "decimals": 18, "source": "native"}],
        "holdings": [{"walletId": "hd:evm:0", "assetId": "eth-mainnet:native", "displayUnits": "1"}],
    }, separators=(",", ":"))


def open_request(at, message="Hello there"):
    snapshot = snapshot_json()
    token = "fintel_chat_" + base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip("=")
    nonce = secrets.token_hex(16)
    timestamp = iso(at)
    proof = "\n".join([
        "FortisNet Intelligence conversation v1", f"Address: {SIGNER.address}", f"Device: {DEVICE_A}",
        f"Nonce: {nonce}", f"Timestamp: {timestamp}", f"Snapshot SHA-256: {sha(snapshot)}",
        f"Session token SHA-256: {sha(token)}"])
    return {"address": SIGNER.address, "nonce": nonce, "timestamp": timestamp, "snapshotHash": sha(snapshot),
            "accessToken": token, "accessTokenHash": sha(token), "snapshotJson": snapshot,
            "signature": SIGNER.sign_message(proof), "firstMessage": message,
            "clientMessageId": secrets.token_hex(16)}


def completed(text):
    return {"json": {"status": "completed", "output": [
        {"type": "message", "content": [{"type": "output_text", "text": text}]}]}}


def call(server, method, path, payload=None, chat=None, headers=None, timeout=60):
    request_headers = {"Authorization": f"Bearer {TOKEN_A}", "Accept": "application/json"}
    body = b""
    if payload is not None:
        body = json.dumps(payload).encode()
        request_headers["Content-Type"] = "application/json"
    if chat:
        request_headers["X-Intelligence-Authorization"] = f"Bearer {chat}"
    request_headers.update(headers or {})
    status, response_headers, data = server.request(method, path, body=body, headers=request_headers,
                                                    timeout=timeout)
    try:
        parsed = json.loads(data) if data else None
    except ValueError:
        parsed = None
    return status, response_headers, parsed, data



def production_gating(binary, root):
    (root / "gating-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "gating-tls", "DNS:localhost")

    def start(env):
        port = harness.free_port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith(("FORTIS_", "AI_", "ANTHROPIC_")) and
                       k not in ("SUPPORT_AUTH_SECRET", "OPENAI_API_KEY", "OPENAI_MODEL")}
        environment.update({"FORTIS_KV_DIR": str(root / f"kv-{port}"), "PORT": str(port)})
        environment.update(env)
        (root / f"kv-{port}").mkdir()
        process = subprocess.Popen([binary, str(port)], env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        return port, process

    def wait_exit(env):
        port, process = start(env)
        try:
            code = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            return None, b""
        return code, process.stderr.read()

    code, stderr = wait_exit({"FORTIS_NATIVE_AI_CANARY": "2"})
    check(code == 2 and b"FORTIS_NATIVE_AI_CANARY must be 0 or 1" in stderr, "canary=2 stops startup")
    code, stderr = wait_exit({"FORTIS_NATIVE_AI_CANARY": "1", "SUPPORT_AUTH_SECRET": SECRET})
    check(code == 2 and b"requires FORTIS_CA_BUNDLE_PATH" in stderr, "canary without a CA bundle stops startup")
    code, stderr = wait_exit({"FORTIS_NATIVE_AI_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca)})
    check(code == 2 and b"requires SUPPORT_AUTH_SECRET" in stderr, "canary without a secret stops startup")
    code, stderr = wait_exit({"FORTIS_NATIVE_AI_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
                              "SUPPORT_AUTH_SECRET": "x" * 31})
    check(code == 2 and b"requires SUPPORT_AUTH_SECRET" in stderr, "a 31-character secret stops startup")

    for env, expected in (({}, 404), ({"FORTIS_NATIVE_AI_CANARY": "0"}, 404),
                          ({"FORTIS_NATIVE_AI_CANARY": "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
                            "SUPPORT_AUTH_SECRET": SECRET}, 401)):
        port, process = start(env)
        try:
            server = type("S", (), {"port": port})()
            for _ in range(200):
                try:
                    with __import__("socket").create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.03)
            for method, path in (("POST", "/v1/ai/conversations"),
                                 ("GET", "/v1/ai/conversations/01JZ0000000000000000000000/messages"),
                                 ("POST", "/v1/ai/conversations/01JZ0000000000000000000000/messages")):
                status, _, _ = harness.Server.request(server, method, path, body=b"" if method == "GET" else b"{}",
                                                      headers={"Content-Type": "application/json"}
                                                      if method == "POST" else {})
                check(status == expected, f"{env or 'no canary'}: {method} {path} answers {status}, want {expected}")
        finally:
            process.terminate()
            process.wait(timeout=10)


def post(server, conversation, chat, client_id, text, results=None, index=None):
    result = call(server, "POST", f"/v1/ai/conversations/{conversation}/messages",
                  {"clientMessageId": client_id, "body": text}, chat=chat)
    if results is not None:
        results[index] = result
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="fortis-ai-gating-") as directory:
        production_gating(sys.argv[1], Path(directory))
    print("AI production gating: configuration refusals, disabled routes and enabled authentication passed")


if __name__ == "__main__":
    main()
