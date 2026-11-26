#!/usr/bin/env python3
"""Run the native adapter against deterministic local TLS endpoints."""
import socket, subprocess, sys, tempfile, time
from pathlib import Path
import tls_ca

binary = Path(sys.argv[1]).resolve()
root = Path(tempfile.mkdtemp(prefix='provider-execution-'))
ca, cert, key, _ = tls_ca.create_certificates(root, 'DNS:localhost')
port = tls_ca.free_port()
closed_port = tls_ca.free_port()
server = subprocess.Popen([
    sys.executable, str(Path(__file__).with_name('provider_execution_fixture_server.py')),
    str(port), str(cert), str(key)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        try:
            with socket.create_connection(('127.0.0.1', port), .1):
                break
        except OSError:
            time.sleep(.02)
    result = subprocess.run([str(binary), str(port), str(closed_port), str(ca)],
                            capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise SystemExit(f'provider execution binary exited {result.returncode}\n'
                         f'{result.stdout}\n{result.stderr}')
    print(result.stdout.strip())
finally:
    server.terminate()
    server.wait(timeout=3)
    import shutil
    shutil.rmtree(root, ignore_errors=True)
