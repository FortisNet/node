#!/usr/bin/env python3
"""Local TLS stand-in for Google's OAuth token and Play Integrity decode
endpoints, for tests/play_integrity_google_test.py.

Usage: play_integrity_google_stub.py PORT CERT KEY FIXTURE_DIR

Serves both fixed paths on one TLS port (the client's test seam points
both hosts at localhost:PORT). FIXTURE_DIR/mode holds "<scenario> <run>" for
the next requests; per-run counters restart when <run> changes. Every request is checked independently of the C++ code:
the RS256 assertion is verified with `openssl dgst` against
FIXTURE_DIR/rsa2048.pub.pem, its claims and header are compared exactly,
the decode call must carry exactly the Bearer this stub issued last, and
the requestHash is recomputed here from the documented byte layout. Each
request appends a JSON line to FIXTURE_DIR/stub.log; a failed check
answers 400 {"error":"stub_check_failed"} and logs the reason.

Enrollment tokens (tests/play_integrity_enrollment_socket_test.py) name
their own scenario and binding, because the server mints the challenge:
"c3.<hex scenario>.<hex challenge>.<hex appVersion>.<nonce>" (hex because
the token alphabet has no "_"). Such a decode uses that scenario instead of
FIXTURE_DIR/mode and binds the verdict to that challenge and appVersion.

Slow scenarios for the enrollment route-lock tests: "decode_delay_10s"
answers 500 ten seconds after the decode arrives (the client's 10 s attempt
deadline started earlier, so it reports Timeout or, at worst,
UpstreamUnavailable: both 503 ATTESTATION_UNAVAILABLE); "decode_hold" waits
until FIXTURE_DIR/release exists, then answers a passing verdict (500 if
not released within 9 s). Each decode log line carries "in_flight", the
number of decodes this stub was answering when it arrived (itself included).
"""

import base64
import hashlib
import json
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

PORT, CERT, KEY, FIXTURE = int(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
EMAIL = "play-verifier@fortis-test.iam.gserviceaccount.com"
KEY_ID = "0123456789abcdef0123456789abcdef01234567"
CHALLENGE = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"
APP_VERSION = "1.4.2"
PACKAGE = "io.fortisnet.wallet"
DECODE_PATH = "/v1/io.fortisnet.wallet:decodeIntegrityToken"
GRANT = "urn:ietf:params:oauth:grant-type:jwt-bearer"

lock = threading.Lock()
state = {"issued": 0, "token": None, "decode_calls": {}, "in_flight": 0}


def b64url_decode(text: str) -> bytes:
    if any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" for c in text):
        raise ValueError("not base64url")
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def request_hash(challenge: str, app_version: str) -> str:
    def lp(value: bytes) -> bytes:
        return len(value).to_bytes(4, "big") + value
    preimage = (lp(b"fortisnet.play-integrity.enroll-request.v1") + lp(challenge.encode()) +
                lp(b"android") + lp(PACKAGE.encode()) + lp(app_version.encode()))
    return base64.urlsafe_b64encode(hashlib.sha256(preimage).digest()).rstrip(b"=").decode()


def verdict(mode: str, challenge: str = CHALLENGE, app_version: str = APP_VERSION) -> bytes:
    certificate = (FIXTURE / "certificate.b64").read_text().strip()
    app, device, licensing = "PLAY_RECOGNIZED", ["MEETS_DEVICE_INTEGRITY"], "LICENSED"
    version_code, stamp, bound = "42", int(time.time() * 1000) - 500, challenge
    if mode == "decode_wrong_cert":
        certificate = base64.urlsafe_b64encode(b"\x07" * 32).rstrip(b"=").decode()
    if mode == "decode_replayed":  # Google's answer to a token decoded before
        app, device, licensing = "UNEVALUATED", [], "UNEVALUATED"
    if mode == "decode_low_version":
        version_code = "39"
    if mode == "decode_basic_only":
        device = ["MEETS_BASIC_INTEGRITY"]
    if mode == "decode_stale":
        stamp -= 301000
    if mode == "decode_other_challenge":
        bound = CHALLENGE if challenge != CHALLENGE else "B" + CHALLENGE[1:]
    payload = {"tokenPayloadExternal": {
        "requestDetails": {"requestPackageName": PACKAGE,
                           "requestHash": request_hash(bound, app_version),
                           "timestampMillis": str(stamp)},
        "appIntegrity": {"appRecognitionVerdict": app, "packageName": PACKAGE,
                         "certificateSha256Digest": [certificate], "versionCode": version_code},
        "deviceIntegrity": {"deviceRecognitionVerdict": device},
        "accountDetails": {"appLicensingVerdict": licensing}}}
    return json.dumps(payload).encode()


def check_assertion(assertion: str) -> str | None:
    parts = assertion.split(".")
    if len(parts) != 3:
        return "assertion is not three segments"
    header, claims = json.loads(b64url_decode(parts[0])), json.loads(b64url_decode(parts[1]))
    signature = b64url_decode(parts[2])
    if header != {"alg": "RS256", "typ": "JWT", "kid": KEY_ID}:
        return f"header {header}"
    if set(claims) != {"iss", "scope", "aud", "exp", "iat"}:
        return f"claims {sorted(claims)}"
    if (claims["iss"] != EMAIL or claims["scope"] != "https://www.googleapis.com/auth/playintegrity"
            or claims["aud"] != "https://oauth2.googleapis.com/token"):
        return "claim values"
    if claims["exp"] - claims["iat"] != 300 or abs(claims["iat"] - time.time()) > 120:
        return "claim times"
    with tempfile.TemporaryDirectory() as scratch:
        data, sig = Path(scratch, "data"), Path(scratch, "sig")
        data.write_bytes(f"{parts[0]}.{parts[1]}".encode())
        sig.write_bytes(signature)
        verified = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(FIXTURE / "rsa2048.pub.pem"),
             "-signature", str(sig), str(data)], capture_output=True)
        if verified.returncode != 0:
            return "RS256 signature does not verify"
    return None


def headers_of(head: bytes) -> tuple[str, str, list[tuple[str, str]]]:
    lines = head.decode("latin-1").split("\r\n")
    method, path, _ = lines[0].split(" ", 2)
    fields = []
    for line in lines[1:]:
        name, _, value = line.partition(":")
        fields.append((name.strip().lower(), value.strip()))
    return method, path, fields


def only(fields, name):
    values = [value for key, value in fields if key == name]
    return values[0] if len(values) == 1 else None


def respond(conn, status: str, body: bytes = b"", content_type="application/json; charset=UTF-8",
            extra: bytes = b"", declared: int | None = None):
    length = len(body) if declared is None else declared
    head = (f"HTTP/1.1 {status}\r\nContent-Length: {length}\r\nConnection: close\r\n").encode()
    if content_type:
        head += f"Content-Type: {content_type}\r\n".encode()
    conn.sendall(head + extra + b"\r\n" + body)


def log(entry: dict):
    with lock, open(FIXTURE / "stub.log", "a") as output:
        output.write(json.dumps(entry) + "\n")


def handle(conn):
    data = b""
    while b"\r\n\r\n" not in data and len(data) < 65536:
        part = conn.recv(4096)
        if not part:
            return
        data += part
    head, _, body = data.partition(b"\r\n\r\n")
    method, path, fields = headers_of(head)
    length = int(only(fields, "content-length") or 0)
    while len(body) < length:
        part = conn.recv(65536)
        if not part:
            break
        body += part
    run_id = (FIXTURE / "mode").read_text().strip()
    mode = run_id.split()[0]
    entry = {"mode": mode, "path": path}

    def fail(reason):
        entry.update(ok=False, reason=reason)
        log(entry)
        respond(conn, "400 Bad Request", b'{"error":"stub_check_failed"}')

    if only(fields, "host") != f"localhost:{PORT}" or method != "POST":
        return fail("host or method")
    if path == "/token":
        if only(fields, "content-type") != "application/x-www-form-urlencoded":
            return fail("oauth content type")
        if only(fields, "authorization") is not None:
            return fail("oauth carries a bearer")
        prefix = b"grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion="
        if not body.startswith(prefix):
            return fail("grant body")
        reason = check_assertion(body[len(prefix):].decode())
        if reason:
            return fail(reason)
        entry.update(ok=True, kind="oauth")
        log(entry)
        if mode == "oauth_500":
            return respond(conn, "500 Internal Server Error", b"{}")
        if mode == "oauth_error":
            return respond(conn, "400 Bad Request",
                           b'{"error":"invalid_grant","error_description":"Invalid JWT Signature."}')
        if mode == "oauth_bad_token":
            return respond(conn, "200 OK",
                           b'{"access_token":"a b","expires_in":3599,"token_type":"Bearer"}')
        with lock:
            state["issued"] += 1
            # Longer than a 255-byte custom header value: the bearer path.
            state["token"] = f"ya29.stub-{state['issued']}." + "x" * 1500
            token = state["token"]
        return respond(conn, "200 OK", json.dumps(
            {"access_token": token, "expires_in": 3599, "token_type": "Bearer",
             "scope": "https://www.googleapis.com/auth/playintegrity"}).encode())

    if path != DECODE_PATH:
        return fail("unexpected path")
    with lock:
        expected = state["token"]
        call = state["decode_calls"][run_id] = state["decode_calls"].get(run_id, 0) + 1
    if only(fields, "authorization") != f"Bearer {expected}":
        return fail("bearer")
    if only(fields, "content-type") != "application/json":
        return fail("decode content type")
    try:
        parsed = json.loads(body)
    except ValueError:
        return fail("decode body json")
    if list(parsed) != ["integrity_token"] or body != json.dumps(
            parsed, separators=(",", ":")).encode():
        return fail("decode body shape")
    binding = {}
    token = parsed["integrity_token"]
    if token.startswith("c3."):
        parts = token.split(".")
        mode = bytes.fromhex(parts[1]).decode()
        binding = {"challenge": bytes.fromhex(parts[2]).decode(),
                   "app_version": bytes.fromhex(parts[3]).decode()}
        entry["mode"] = mode
    with lock:
        state["in_flight"] += 1
        entry.update(ok=True, kind="decode", token_bytes=len(token), in_flight=state["in_flight"])
    try:
        log(entry)
        return answer_decode(conn, mode, call, binding)
    finally:
        with lock:
            state["in_flight"] -= 1


def answer_decode(conn, mode: str, call: int, binding: dict):
    if mode == "decode_429":
        return respond(conn, "429 Too Many Requests", b'{"error":{"code":429}}')
    if mode == "decode_400":
        return respond(conn, "400 Bad Request", b'{"error":{"code":400,"status":"INVALID_ARGUMENT"}}')
    if mode == "decode_401_once" and call == 1:
        return respond(conn, "401 Unauthorized",
                       b'{"error":{"code":401,"status":"UNAUTHENTICATED"}}')
    if mode == "decode_403":
        return respond(conn, "403 Forbidden", b'{"error":{"code":403,"status":"PERMISSION_DENIED"}}')
    if mode == "decode_500":
        return respond(conn, "500 Internal Server Error", b"{}")
    if mode == "decode_redirect":
        return respond(conn, "302 Found", b"", extra=b"Location: https://localhost/evil\r\n")
    if mode == "decode_truncated":
        return respond(conn, "200 OK", verdict(mode)[:100], declared=5000)
    if mode == "decode_oversized":
        return respond(conn, "200 OK", b" " * 16385)
    if mode == "decode_html":
        return respond(conn, "200 OK", verdict(mode, **binding), content_type="text/html")
    if mode == "decode_error_json":
        return respond(conn, "200 OK", b'{"error":{"code":400,"message":"cannot decode"}}')
    if mode == "decode_slow":
        time.sleep(11)
    if mode == "decode_delay_10s":
        time.sleep(10)
        return respond(conn, "500 Internal Server Error", b"{}")
    if mode == "decode_hold":
        deadline = time.monotonic() + 9
        while not (FIXTURE / "release").exists():
            if time.monotonic() > deadline:
                return respond(conn, "500 Internal Server Error", b"{}")
            time.sleep(0.01)
    return respond(conn, "200 OK", verdict(mode, **binding))


def serve(raw, context):
    try:
        with context.wrap_socket(raw, server_side=True) as conn:
            conn.settimeout(20)
            handle(conn)
    except (OSError, ssl.SSLError, ValueError):
        pass


def main():
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(CERT, KEY)
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", PORT))
    listener.listen(32)
    while True:
        raw, _ = listener.accept()
        threading.Thread(target=serve, args=(raw, context), daemon=True).start()


if __name__ == "__main__":
    main()
