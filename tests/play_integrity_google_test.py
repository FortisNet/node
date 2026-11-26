#!/usr/bin/env python3
"""Play Integrity client: unit groups, local TLS stub and release isolation.

Usage: play_integrity_google_test.py TEST_BINARY RELEASE_TEST RELEASE_OBJECT
                                     SEAM_OBJECT RELEASE_SERVER

1. Generates every key, CA and certificate at test time with openssl in a
   temporary directory (nothing private is stored in Git): PKCS#8 RSA
   2048/3072, RSA 1024, EC P-256, a PKCS#1 RSA key, an encrypted PKCS#8 key,
   and a TLS CA with a localhost server certificate.
2. Runs the in-process unit groups (TEST_BINARY unit).
3. Runs one decode per scenario over real TLS against
   tests/play_integrity_google_stub.py, which verifies the RS256 assertion
   with openssl, the exact Bearer, path and body, and recomputes the
   requestHash itself; also a wrong CA and a wrong peer name.
4. Runs the release-build test and checks that the release object and the
   server binary hold no test seam. The server binary and the
   guest source list are expected to hold the release client (and only it);
   tests/play_integrity_enrollment_socket_test.py checks the enrollment side.
"""

import base64
import json
import os
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import tls_ca

ROOT = Path(__file__).resolve().parent.parent
TOKEN = "eyJhbGciOiJBMjU2S1ciLCJlbmMiOiJBMjU2R0NNIn0.tls-{}.iv_-.cipher.tag"


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_fixture(root: Path) -> tuple[Path, Path, Path, Path]:
    for name, options in (("rsa2048", ["-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048"]),
                          ("rsa3072", ["-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:3072"]),
                          ("rsa1024", ["-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:1024"]),
                          ("ec", ["-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256"])):
        run("openssl", "genpkey", *options, "-out", str(root / f"{name}.pem"))
    for name in ("rsa2048", "rsa3072"):
        run("openssl", "pkey", "-in", str(root / f"{name}.pem"), "-pubout",
            "-out", str(root / f"{name}.pub.pem"))
    run("openssl", "rsa", "-in", str(root / "rsa2048.pem"), "-traditional",
        "-out", str(root / "rsa_pkcs1.pem"))
    run("openssl", "pkcs8", "-topk8", "-in", str(root / "rsa2048.pem"), "-v2", "aes-256-cbc",
        "-passout", "pass:fortis-test", "-out", str(root / "encrypted.pem"))
    assert (root / "rsa2048.pem").read_text().startswith("-----BEGIN PRIVATE KEY-----\n")
    assert (root / "rsa_pkcs1.pem").read_text().startswith("-----BEGIN RSA PRIVATE KEY-----")
    assert (root / "encrypted.pem").read_text().startswith("-----BEGIN ENCRYPTED PRIVATE KEY")
    certificate = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    (root / "certificate.b64").write_text(certificate + "\n")
    return tls_ca.create_certificates(root, "DNS:localhost")


def start_stub(port: int, cert: Path, key: Path, fixture: Path) -> subprocess.Popen:
    stub = subprocess.Popen([sys.executable, str(Path(__file__).with_name(
        "play_integrity_google_stub.py")), str(port), str(cert), str(key), str(fixture)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(200):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return stub
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("stub did not listen")


def stub_log(fixture: Path) -> list[dict]:
    path = fixture / "stub.log"
    entries = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    path.unlink(missing_ok=True)
    return entries


def tls_scenarios(binary: str, fixture: Path, ca: Path, other_ca: Path, port: int) -> None:
    counter = [0]

    def decode(mode: str, host="localhost", ca_file=ca, app_version="1.4.2") -> list[str]:
        counter[0] += 1
        (fixture / "mode").write_text(f"{mode} {counter[0]}\n")
        started = time.monotonic()
        result = subprocess.run([binary, "tls", str(fixture), host, str(port), str(ca_file),
                                 app_version, TOKEN.format(counter[0])],
                                capture_output=True, text=True, timeout=40)
        assert result.returncode == 0, (mode, result.stdout, result.stderr)
        words = result.stdout.split()
        print(f"  tls {mode:<20} {' '.join(words):<55} {time.monotonic() - started:5.2f} s")
        return words

    def expect(mode, words, oauth_entries, decode_entries, **kwargs):
        got = decode(mode, **kwargs)
        assert got[:len(words)] == words, (mode, got)
        log = stub_log(fixture)
        failures = [entry for entry in log if not entry.get("ok")]
        assert not failures, (mode, failures)
        assert sum(entry.get("kind") == "oauth" for entry in log) == oauth_entries, (mode, log)
        assert sum(entry.get("kind") == "decode" for entry in log) == decode_entries, (mode, log)
        return log

    stub_log(fixture)
    log = expect("ok", ["Verified", "Verified", "1", "1", "Replayed", "1"], 1, 1)
    assert log[1]["token_bytes"] == len(TOKEN.format(counter[0]))
    expect("decode_401_once", ["Verified", "Verified", "2", "2"], 2, 2)
    expect("decode_403", ["PermissionDenied", "PolicyInvalid", "1", "1"], 1, 1)
    expect("decode_500", ["UpstreamUnavailable", "PolicyInvalid", "1", "1"], 1, 1)
    # No redirect is followed: one decode request, no request to /evil.
    expect("decode_redirect", ["UpstreamRejected", "PolicyInvalid", "1", "1"], 1, 1)
    expect("decode_truncated", ["ResponseMalformed", "PolicyInvalid", "1", "1"], 1, 1)
    expect("decode_oversized", ["ResponseTooLarge", "PolicyInvalid", "1", "1"], 1, 1)
    expect("decode_html", ["ResponseMalformed", "PolicyInvalid", "1", "1"], 1, 1)
    expect("decode_error_json", ["VerdictRejected", "PayloadMalformed", "1", "1"], 1, 1)
    expect("decode_wrong_cert", ["VerdictRejected", "CertificateMismatch", "1", "1"], 1, 1)
    expect("decode_replayed", ["VerdictRejected", "AppNotRecognized", "1", "1"], 1, 1)
    # The stub's verdict is bound to appVersion 1.4.2.
    expect("ok", ["VerdictRejected", "RequestHashMismatch", "1", "1"], 1, 1, app_version="1.4.3")
    expect("oauth_500", ["OAuthUnavailable", "PolicyInvalid", "1", "0"], 1, 0)
    expect("oauth_error", ["OAuthRejected", "PolicyInvalid", "1", "0"], 1, 0)
    expect("oauth_bad_token", ["OAuthMalformed", "PolicyInvalid", "1", "0"], 1, 0)
    # TLS trust and peer name: the handshake fails before any HTTP request.
    expect("ok", ["OAuthUnavailable", "PolicyInvalid", "1", "0"], 0, 0, ca_file=other_ca)
    expect("ok", ["OAuthUnavailable", "PolicyInvalid", "1", "0"], 0, 0, host="127.0.0.1")
    expect("decode_slow", ["Timeout", "PolicyInvalid", "1", "1"], 1, 1)


def symbols(path: str) -> str:
    return subprocess.run(["nm", "-C", path], check=True, capture_output=True, text=True).stdout


def release_isolation(release_test: str, release_object: str, seam_object: str, server: str,
                      ca: Path, fixture: Path) -> None:
    subprocess.run([release_test], check=True)
    account = fixture / "accounts" / "tls.json"
    assert account.exists() and (account.stat().st_mode & 0o077) == 0
    subprocess.run([release_test, str(ca), str(account)], check=True)

    release_symbols, seam_symbols = symbols(release_object), symbols(seam_object)
    for name in ("allow_test_loopback", "PlayIntegrityGoogleTestSeam"):
        assert name not in release_symbols, name
    assert "allow_test_loopback" in seam_symbols
    assert "PlayIntegrityDecodeSource" in release_symbols or "VerifyPlayIntegrityVerdict" in release_symbols
    server_bytes = Path(server).read_bytes()
    server_symbols = symbols(server)
    # The release client is linked into the server...
    for name in ("PlayIntegrityGoogleClient::Decode", "VerifyPlayIntegrityVerdict",
                 "LoadPlayServiceAccount"):
        assert name in server_symbols, name
    for text in (b"playintegrity.googleapis.com", b"oauth2.googleapis.com"):
        assert text in server_bytes, text
    # ...and never the seam: no seam type, and the only allow_test_loopback
    # is provider_http_bridge's own (runtime-gated) one.
    assert "PlayIntegrityGoogleTestSeam" not in server_symbols
    bridge = symbols(str(ROOT / "build/provider_http_bridge.o")).count("allow_test_loopback(")
    assert server_symbols.count("allow_test_loopback(void*, unsigned int)") == bridge == 1
    makefile = (ROOT / "Makefile").read_text()
    assert re.search(r"^PLAY_INTEGRITY_OBJECTS = build/play_integrity\.o "
                     r"build/play_integrity_google\.o build/play_integrity_enrollment\.o$",
                     makefile, re.M)
    # The guest's source list is sources.mk, which its build includes.
    uk = (ROOT / "sources.mk").read_text()
    assert "src/auth/play_integrity.cpp" in uk and "src/auth/play_integrity_google.cpp" in uk
    assert "FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT" not in uk and "_test_seam" not in uk
    header = (ROOT / "src/auth/play_integrity_google.h").read_text()
    assert ("#if defined(FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT) && defined(FORTIS_UNIKRAFT)\n#error"
            in header)
    # One PlayIntegrityDecodeSource definition in the server sources.
    defining = [path.name for path in (ROOT / "src").rglob("*.cpp")
                if re.search(r"^class PlayIntegrityDecodeSource\s*\{", path.read_text(), re.M)]
    assert defining == ["play_integrity_google.cpp"], defining
    print("release: no seam in the release object or server binary; release client in the "
          "server and Makefile.uk without the test flag; one decoded-response factory: ok")


def main() -> None:
    binary, release_test, release_object, seam_object, server = (
        str(Path(arg).resolve()) for arg in sys.argv[1:6])
    with tempfile.TemporaryDirectory(prefix="play-c2-") as temporary:
        fixture = Path(temporary)
        os.chmod(fixture, 0o700)
        ca, cert, key, other_ca = make_fixture(fixture)
        started = time.monotonic()
        subprocess.run([binary, "unit", str(fixture)], check=True, timeout=600)
        print(f"unit groups: {time.monotonic() - started:.1f} s")
        port = tls_ca.free_port()
        stub = start_stub(port, cert, key, fixture)
        try:
            tls_scenarios(binary, fixture, ca, other_ca, port)
        finally:
            stub.terminate()
            stub.wait(timeout=5)
        release_isolation(release_test, release_object, seam_object, server, ca, fixture)
    print("play_integrity_google_test.py: passed")


if __name__ == "__main__":
    main()
