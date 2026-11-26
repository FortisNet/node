#!/usr/bin/env python3
"""The former Helpdesk and staging admin routes stay absent from C++."""

import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def status(url):
    try:
        with urllib.request.urlopen(url, timeout=2) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def main(binary):
    with tempfile.TemporaryDirectory() as data:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        env = os.environ.copy()
        env.update(FORTIS_KV_DIR=data, FORTIS_NATIVE_SUPPORT_CANARY="1",
                   SUPPORT_ADMIN_TOKEN="former-staging-credential")
        server = subprocess.Popen([binary, str(port)], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            base = f"http://127.0.0.1:{port}"
            for _ in range(100):
                if server.poll() is not None:
                    raise AssertionError(f"server exited: {server.stderr.read().decode()[-500:]}")
                try:
                    if status(base + "/healthz") == 200:
                        break
                except (OSError, TimeoutError):
                    time.sleep(0.1)
            else:
                raise AssertionError("server did not start")
            for path in ("/v1/support/tickets", "/v1/support/auth/verify",
                         "/admin/tickets", "/internal/support/v1/tickets"):
                assert status(base + path) == 404, path
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/fortis_node")
