#!/usr/bin/env python3
"""Server-Timing on every native response, against the real composition with
the async test probes (build/fortis_node_probe_test: the production
server plus the replay binaries' /probe/async and /probe/async-timeout) and
/metrics.

The case list lives in server_timing.py and is shared with guest checks:
success and HEAD (bodyless, same Content-Length), authentication and
validation refusals, parser errors (400/431/413), unknown routes, deferred
and reserved handoff responses (including a 504) and /metrics (route
counters still present). Each response must carry exactly one
`Server-Timing: app;dur=<ms>` line; a delayed handler's value must grow with
its delay. No credential reaches process output. The support staff bridge
was removed with the Helpdesk (01882966): its public paths are the router's
404.

Usage: server_timing_native_test.py <fortis_node_probe_test>
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import server_timing

HOST = "127.0.0.1"
METRICS = "timing-metrics-token-" + os.urandom(8).hex()


def free_port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def wait_listening(process, port):
    for _ in range(300):
        if process.poll() is not None:
            raise RuntimeError("fortis_node exited during startup")
        try:
            with socket.create_connection((HOST, port), timeout=.1):
                return
        except OSError:
            time.sleep(.03)
    raise RuntimeError(f"port {port} never accepted")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/fortis_node_probe_test"
    with tempfile.TemporaryDirectory(prefix="fortis-timing-") as directory:
        root = Path(directory)
        (root / "kv").mkdir()
        port = free_port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_") and
                       k not in ("SUPPORT_AUTH_SECRET", "SUPPORT_ADMIN_TOKEN", "NODE_ENV", "METRICS_TOKEN")}
        environment.update({
            "PORT": str(port), "FORTIS_KV_DIR": str(root / "kv"), "METRICS_TOKEN": METRICS,
            "FORTIS_ENABLE_ASYNC_TEST_PROBE": "1"})
        with open(root / "out", "wb") as out:
            process = subprocess.Popen([binary, str(port)], env=environment, stdout=out, stderr=out)
            try:
                wait_listening(process, port)
                server_timing.public_checks(HOST, port, metrics_token=METRICS)
                # The removed staff bridge's paths are the router's 404.
                response = server_timing.call(HOST, port, "GET", "/internal/support/v1/tickets",
                                              headers={"Authorization": "Bearer timing-token"})
                assert response.status == 404, response.status
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        output = (root / "out").read_bytes()
        for text in (METRICS,):
            assert text.encode() not in output, f"process output contains {text[:16]!r}"
    print("Server-Timing: one parseable line on every public response path, async included")


if __name__ == "__main__":
    main()
