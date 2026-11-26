#!/usr/bin/env python3
"""The release server links only the official App Attest root.

Usage: apple_attest_release_check.py RELEASE_SERVER TEST_ROOT_SERVER

Checks, on the built binaries and the guest source list:
- the release binary embeds the official Apple App Attestation Root CA DER
  (whose SHA-256 is the pinned fingerprint) exactly once;
- it holds no test-root loader (the FORTIS_TEST_APPLE_ATTEST_ROOT name), no
  synthetic CA subject and no test issuer symbol;
- the test-root server does hold the loader name, so the check can fail;
- the guest list compiles the verifier without the test flag, and the
  source refuses that flag in a guest build.
"""

import hashlib
import re
import subprocess
import sys
from pathlib import Path

PINNED = "1cb9823ba28ba6ad2d33a006941de2ae4f513ef1d4e831b9f7e0fa7b6242c932"
ROOT = Path(__file__).resolve().parent.parent


def official_root() -> bytes:
    source = (ROOT / "src/auth/apple_app_attest_root.cpp").read_text()
    array = source.split("kAppleAppAttestRootDer[] = {", 1)[1].split("};", 1)[0]
    der = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", array))
    assert hashlib.sha256(der).hexdigest() == PINNED, hashlib.sha256(der).hexdigest()
    return der


def symbols(binary: str) -> str:
    return subprocess.run(["nm", "-C", binary], check=True, capture_output=True, text=True).stdout


def main() -> None:
    release, test_root = sys.argv[1], sys.argv[2]
    der = official_root()
    release_bytes = Path(release).read_bytes()
    test_bytes = Path(test_root).read_bytes()
    assert release_bytes.count(der) == 1, release_bytes.count(der)
    for forbidden in (b"FORTIS_TEST_APPLE_ATTEST_ROOT", b"Fortis Synthetic App Attest",
                      b"Fortis Test"):
        assert forbidden not in release_bytes, forbidden
    assert b"FORTIS_TEST_APPLE_ATTEST_ROOT" in test_bytes
    release_symbols = symbols(release)
    for forbidden in ("load_test_root", "DeviceAttestationIssuer::Make", "apple_fixture"):
        assert forbidden not in release_symbols, forbidden
    assert "load_test_root" in symbols(test_root)
    issuers = sorted({line.split(" ", 2)[2] for line in release_symbols.splitlines()
                      if "DeviceAttestationIssuer" in line})
    assert all("IssueApple" in name for name in issuers), issuers
    # The guest's source list is sources.mk, which its build includes.
    uk = (ROOT / "sources.mk").read_text()
    assert "src/auth/apple_app_attest.cpp" in uk and "src/auth/apple_app_attest_root.cpp" in uk
    assert "FORTIS_APPLE_ATTEST_TEST_ROOT" not in uk
    verifier = (ROOT / "src/auth/apple_app_attest.cpp").read_text()
    assert ("#if defined(FORTIS_APPLE_ATTEST_TEST_ROOT) && defined(FORTIS_UNIKRAFT)\n#error"
            in verifier)
    print(f"release binary: official root (sha256 {PINNED[:16]}...) embedded once, no test "
          f"root loader, synthetic CA or test issuer; issuer symbols {issuers or 'inlined'}; "
          "guest list compiles the release verifier only: ok")


if __name__ == "__main__":
    main()
