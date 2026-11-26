#!/usr/bin/env python3
"""AI socket fixture and current-contract authentication checks.

The recorded response replay is not run: its synchronous OpenAI replies
and seven-day expiry no longer exist. The signed
proof fixture remains pinned; current behaviour is tested by the AI route,
durable-thread and asynchronous Anthropic suites.
"""
import hashlib
import json
import os
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import discovery_route_harness as harness
import tls_ca

FIXTURE = "ai_conversations_route.json"
PROVIDER_TIMEOUT_MS = 1500


class OpenAi:
    """A generated-CA HTTPS stand-in for api.openai.com/v1/responses."""

    def __init__(self, root):
        self.lock = threading.Lock()
        self.script = []
        self.observed = []
        # Native tests: the step answering when the script is empty.
        self.default = None
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length)
                headers = {k.lower(): v for k, v in self.headers.items()}
                call = {"method": "POST", "path": self.path, "headers": headers, "body": body,
                        "start": time.monotonic(), "end": None}
                with upstream.lock:
                    upstream.observed.append(call)
                    step = upstream.script.pop(0) if upstream.script else upstream.default
                try:
                    self.answer(step)
                finally:
                    call["end"] = time.monotonic()

            def answer(self, step):
                if step is None:
                    self.reply(599, b"unexpected")
                    return
                if step.get("timeout"):
                    time.sleep(step.get("stallMs", PROVIDER_TIMEOUT_MS + 1000) / 1000)
                    self.close_connection = True
                    return
                time.sleep((step.get("delayMs") or 5) / 1000)
                text = step["text"] if "text" in step else json.dumps(step["json"], ensure_ascii=False,
                                                                       separators=(",", ":"))
                self.reply(step.get("status", 200), text.encode("utf-8", "surrogatepass"))

            def reply(self, status, body):
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", harness.free_port()), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def load(self, steps):
        with self.lock:
            self.script = [dict(step) for step in steps]

    def calls(self):
        with self.lock:
            return list(self.observed)

    def remaining(self):
        with self.lock:
            return len(self.script)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Server:
    """One AI test process on the shared KV directory and id counter."""

    def __init__(self, binary, root, fixture, upstream, extra_env=None):
        self.port = harness.free_port()
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("FORTIS_", "AI_", "ANTHROPIC_")) and key not in (
                           "OPENAI_API_KEY", "OPENAI_MODEL", "SUPPORT_AUTH_SECRET")}
        environment.update(fixture["env"])
        environment.update({
            "FORTIS_NATIVE_AI_CANARY": "1",
            "FORTIS_CA_BUNDLE_PATH": str(upstream.ca),
            "FORTIS_ENABLE_TEST_AI_ROUTES": "1",
            "FORTIS_TEST_AI_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_AI_NOW_FILE": str(root / "clock"),
            "FORTIS_TEST_AI_ID_FILE": str(root / "ids"),
            "FORTIS_TEST_AI_PROVIDER_TIMEOUT_MS": str(PROVIDER_TIMEOUT_MS),
            "FORTIS_KV_DIR": str(root / "kv"),
            "PORT": str(self.port),
        })
        environment.update(extra_env or {})
        environment = {key: value for key, value in environment.items() if value is not None}
        self.stderr = open(root / "stderr.log", "ab")
        self.stdout = open(root / "stdout.log", "ab")
        self.process = subprocess.Popen([binary, str(self.port)], env=environment, stdout=self.stdout,
                                        stderr=self.stderr)
        for _ in range(300):
            if self.process.poll() is not None:
                error = (root / "stderr.log").read_text()
                self.stop()
                raise RuntimeError("test server exited: " + error)
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        self.stop()
        raise RuntimeError("test server did not start")

    def kill(self):
        self.process.send_signal(signal.SIGKILL)
        self.process.wait()
        self.stderr.close()
        self.stdout.close()

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.stderr.close()
        self.stdout.close()

    request = harness.Server.request


def load_fixture(name):
    """The fixture, after checking its pinned `sha256  name` line."""
    data = (harness.FIXTURES / name).read_bytes()
    pinned = (harness.FIXTURES / f"{name}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{name}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


def main():
    from ai_current_contract import auth_order
    auth_order(sys.argv[1])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
