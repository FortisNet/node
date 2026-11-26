#!/usr/bin/env python3
"""Verify a generated CA, then reject an untrusted CA and wrong hostname."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def create_certificates(root: Path, san: str) -> tuple[Path, Path, Path, Path]:
    ca = root / "ca.pem"
    key = root / "ca.key"
    server_key = root / "server.key"
    server_csr = root / "server.csr"
    server_cert = root / "server.pem"
    extensions = root / "server.ext"
    extensions.write_text("basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
                          f"extendedKeyUsage=serverAuth\nsubjectAltName={san}\n")
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
        "-days", "2", "-subj", "/CN=Fortis TLS test CA", "-addext",
        "basicConstraints=critical,CA:TRUE", "-addext",
        "keyUsage=critical,keyCertSign,cRLSign", "-keyout", str(key), "-out", str(ca))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-sha256",
        "-subj", "/CN=localhost", "-keyout", str(server_key), "-out", str(server_csr))
    run("openssl", "x509", "-req", "-in", str(server_csr), "-CA", str(ca),
        "-CAkey", str(key), "-CAcreateserial", "-days", "2", "-sha256",
        "-extfile", str(extensions), "-out", str(server_cert))
    other_ca = root / "other-ca.pem"
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-days", "2", "-subj", "/CN=Untrusted test CA", "-keyout",
        str(root / "other-ca.key"), "-out", str(other_ca))
    return ca, server_cert, server_key, other_ca


def probe(binary: str, port: int, host: str, tls_port: int, ca: Path) -> tuple[int, dict]:
    env = os.environ.copy()
    for name in ("FORTIS_ENABLE_PROVIDER_TEST_ROUTE", "FORTIS_TEST_UPSTREAM_PORT",
                 "FORTIS_TEST_TLS_FAILURE_PORT", "FORTIS_TEST_UPSTREAM_CA"):
        env.pop(name, None)
    env.update(FORTIS_ENABLE_DIAGNOSTIC_PROBES="1",
               FORTIS_TLS_PROBE_HOST=host, FORTIS_TLS_PROBE_PORT=str(tls_port),
               FORTIS_TLS_PROBE_CA=str(ca))
    server = subprocess.Popen([binary, str(port)], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if server.poll() is not None:
                raise RuntimeError("native server exited")
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/probe/tls", timeout=8) as response:
                    return response.status, json.load(response)
            except urllib.error.HTTPError as error:
                return error.code, json.load(error)
            except urllib.error.URLError:
                time.sleep(0.02)
        raise RuntimeError("native server did not listen")
    finally:
        server.terminate()
        server.wait(timeout=3)


def main() -> None:
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        ca, server_cert, server_key, other_ca = create_certificates(root, "DNS:localhost")

        tls_port = free_port()
        tls_server = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("outbound_https_fixture_server.py")),
             str(tls_port), str(server_cert), str(server_key)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", tls_port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise RuntimeError("TLS fixture did not listen")

            status, valid = probe(binary, free_port(), "localhost", tls_port, ca)
            assert status == 200 and valid["connected"] and valid["verified"] and valid["response"], valid

            status, invalid_ca = probe(binary, free_port(), "localhost", tls_port, other_ca)
            assert status == 503 and invalid_ca["connected"] and not invalid_ca["verified"], invalid_ca

            status, invalid_name = probe(binary, free_port(), "127.0.0.1", tls_port, ca)
            assert status == 503 and invalid_name["connected"] and not invalid_name["verified"], invalid_name
            bad_bundle = root / "invalid-bundle.pem"
            bad_bundle.write_text("not a PEM CA bundle\n")
            bad_env = os.environ.copy()
            bad_env["FORTIS_CA_BUNDLE_PATH"] = str(bad_bundle)
            invalid_start = subprocess.run([binary, str(free_port())], env=bad_env,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                           timeout=3)
            assert invalid_start.returncode == 2, invalid_start.returncode
            print("generated CA TLS validation, untrusted CA, hostname rejection, and startup CA validation passed")
        finally:
            tls_server.terminate()
            tls_server.wait(timeout=3)


if __name__ == "__main__":
    main()
