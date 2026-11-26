#!/usr/bin/env python3
"""Play Integrity client mutation check (run from the repository root).

Usage: play_integrity_google_mutants.py [NAME ...]

Each mutant edits one security check in a scratch copy of
src/auth/play_integrity_google.cpp or src/http/outbound_https.cpp, builds a fresh
test binary from that copy in a temporary directory (the tree and build/
are never touched, so no stale object can hide a mutant), and runs
tests/play_integrity_google_test.py with it. A mutant must make the suite
fail ("killed"); one that passes is reported and the script exits 1.
Requires the objects `make test-play-integrity` builds.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOOGLE = "src/auth/play_integrity_google.cpp"
OUTBOUND = "src/http/outbound_https.cpp"

# name: (file, original text, mutated text) or (file, [(original, mutated), ...])
MUTANTS = {
    "permissions": (GOOGLE, "else if (status.st_mode & 077)", "else if (status.st_mode & 0)"),
    "pkcs8_label": (GOOGLE, "memcmp(pem, kPkcs8Begin, sizeof(kPkcs8Begin) - 1) == 0 &&\n"
                    "      (pem[sizeof(kPkcs8Begin) - 1] == '\\n' || pem[sizeof(kPkcs8Begin) - 1] == '\\r') &&",
                    "true &&"),
    "rsa_bits": (GOOGLE, "mbedtls_pk_get_bitlen(key) >= kServiceAccountMinRsaBits &&", "true &&"),
    "token_uri": (GOOGLE, "!document.StringEquals(members[kTokenUri].value, kGoogleOAuthAudience))",
                  "false)"),
    "jwt_lifetime": (GOOGLE, "now_seconds + kPlayOAuthAssertionLifetimeSeconds)",
                     "now_seconds + 3600)"),
    "oauth_error_member": (GOOGLE,
                           "if (members[kError].value != codec::kBoundedJsonNoToken) return GoogleOAuthParse::ErrorResponse;",
                           ""),
    "token_type": (GOOGLE, '!document.StringEquals(members[kType].value, "Bearer", true) ||', ""),
    "oauth_media_type": (GOOGLE, "          json_media_type(response)\n              ?",
                         "          true\n              ?"),
    "refresh_margin": (GOOGLE, "refresh_at_ms_ = started + lifetime - margin;",
                       "refresh_at_ms_ = started + lifetime;"),
    "single_flight": (GOOGLE, "    if (!refreshing_) {\n      if (now < failure_until_ms_)",
                      "    if (true) {\n      if (now < failure_until_ms_)"),
    "failure_backoff": (GOOGLE, "failure_until_ms_ = MonotonicMs() + kPlayOAuthFailureBackoffMs;",
                        "failure_until_ms_ = 0;"),
    "stale_invalidate": (GOOGLE, "if (have_token_ && generation_ == generation) {",
                         "if (have_token_) {"),
    "no_401_retry": (GOOGLE, "if (response.status == 401 && attempt == 0) {",
                     "if (response.status == 401 && attempt == 9) {"),
    "accept_non_2xx": (GOOGLE, "if (response.status < 200 || response.status > 299) {",
                       "if (response.status < 200 || response.status > 599) {"),
    "decode_media_type": (GOOGLE, "    if (!json_media_type(response)) {", "    if (false) {"),
    "replay_guard": (GOOGLE,
                     "if (replay_[i].expires_ms > now && memcmp(replay_[i].digest, digest, 32) == 0) {",
                     "if (false) {"),
    "replay_capacity": (GOOGLE, "    if (slot.expires_ms > now) {", "    if (false) {"),
    "request_hash_version": (GOOGLE, "input.app_version, input.app_version_length,",
                             '"1.4.2", 5,'),
    "concurrency_cap": (GOOGLE, "const bool admitted = in_flight_ < kPlayDecodeMaxConcurrent;",
                        "const bool admitted = true;"),
    "token_charset": (GOOGLE, "c != '+' && c != '/' && c != '=')\n      return false;",
                      "c != '+' && c != '/' && c != '=' && c != '\"' && c != ' ')\n      return false;"),
    "bearer_crlf": (OUTBOUND, "  if (i == 0) return false;\n  for (; i < length; ++i)\n    if (value[i] != '=') return false;\n  for (size_t h",
                    "  if (i == 0) return false;\n  for (size_t h"),
    "bearer_plaintext": (OUTBOUND, "(request.authorization_bearer && request.plaintext) ||", ""),
    # Both TLS trust checks at once: no chain verification in the handshake
    # and no post-handshake verify-result check.
    "tls_verify": (OUTBOUND, [
        ("mbedtls_ssl_conf_authmode(&session->config, MBEDTLS_SSL_VERIFY_REQUIRED);",
         "mbedtls_ssl_conf_authmode(&session->config, MBEDTLS_SSL_VERIFY_NONE);"),
        ("  if (rc == 0 && mbedtls_ssl_get_verify_result(&session->ssl) != 0)\n"
         "    rc = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;", "")]),
}

FLAGS = ["-O2", "-std=c++17", "-Wall", "-Wextra", "-pedantic", "-ffp-contract=off",
         "-DPB_FIELD_32BIT", "-DMBEDTLS_THREADING_C", "-DMBEDTLS_THREADING_PTHREAD", "-pthread",
         "-I.deps/mbedtls-3.6.7/include", "-Isrc", "-Isrc/crypto",
         '-DMBEDTLS_USER_CONFIG_FILE="fortis_mbedtls_config.h"',
         "-DFORTIS_PLAY_INTEGRITY_TEST_TRANSPORT"]
LIBS = [".deps/mbedtls-3.6.7/library/libmbedtls.a", ".deps/mbedtls-3.6.7/library/libmbedx509.a",
        ".deps/mbedtls-3.6.7/library/libmbedcrypto.a"]


def build(scratch: Path, mutated: str, text: str) -> Path:
    sources = {GOOGLE: (ROOT / GOOGLE).read_text(), OUTBOUND: (ROOT / OUTBOUND).read_text()}
    sources[mutated] = text
    objects = []
    for source, content in sources.items():
        copy = scratch / Path(source).name
        copy.write_text(content)
        obj = copy.with_suffix(".o")
        subprocess.run(["c++", *FLAGS, "-c", str(copy), "-o", str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    binary = scratch / "play_integrity_google_test"
    subprocess.run(["c++", *FLAGS, "tests/play_integrity_google_test.cpp", *objects,
                    "build/play_integrity.o", "build/bounded_json.o", *LIBS, "-o", str(binary)],
                   cwd=ROOT, check=True)
    return binary


def main() -> None:
    names = sys.argv[1:] or list(MUTANTS)
    survivors = []
    for name in names:
        path, *edits = MUTANTS[name]
        pairs = edits[0] if len(edits) == 1 else [tuple(edits)]
        text = (ROOT / path).read_text()
        for original, mutated in pairs:
            assert text.count(original) == 1, (name, text.count(original))
            text = text.replace(original, mutated)
        with tempfile.TemporaryDirectory(prefix=f"play-c2-{name}-") as scratch:
            binary = build(Path(scratch), path, text)
            result = subprocess.run(
                [sys.executable, "tests/play_integrity_google_test.py", str(binary),
                 "build/play_integrity_google_release_test", "build/play_integrity_google.o",
                 "build/play_integrity_google_test_seam.o", "build/fortis_node"],
                cwd=ROOT, capture_output=True, text=True, timeout=900,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        output = (result.stdout + result.stderr).strip().splitlines()
        evidence = next((line for line in output if "check failed" in line or "got " in line or "accepted:" in line
                         or "AssertionError" in line), output[-1] if output else "")
        if result.returncode == 0:
            survivors.append(name)
            print(f"SURVIVED {name}")
        else:
            print(f"killed   {name:<22} {evidence.strip()[:110]}")
    if survivors:
        sys.exit(f"surviving mutants: {survivors}")
    print(f"all {len(names)} mutants killed")


if __name__ == "__main__":
    main()
