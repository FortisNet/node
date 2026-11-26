#!/usr/bin/env python3
"""Core and contract background refresh must bypass a quota-blocked primary."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import tls_ca

with tempfile.TemporaryDirectory(prefix='fortis-fallback-warmer-') as temporary:
    root = Path(temporary)
    ca, cert, key, _ = tls_ca.create_certificates(root, 'DNS:localhost')
    paths = []
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            paths.append(self.path)
            if self.path.startswith('/prices/current/'):
                assert not self.headers.get('x-cg-demo-api-key')
                assert not self.headers.get('x-cg-pro-api-key')
                coins = self.path.removeprefix('/prices/current/').split(',')
                body = json.dumps({'coins': {coin: {'price': 2.5, 'timestamp': time.time(), 'confidence': .99} for coin in coins}}).encode()
                status = 200
            else:
                assert self.path.startswith('/api/v3/simple/'), self.path
                status, body = 429, b'{"status":{"error_code":10006}}'
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try: self.wfile.write(body)
            except OSError: pass
        def log_message(self, *_): pass
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([sys.argv[1], str(server.server_port), str(ca)], check=True, timeout=30)
        fallback = [path for path in paths if path.startswith('/prices/current/')]
        assert len(fallback) >= 4, paths
        assert sum('coingecko:bitcoin' in path for path in fallback) >= 2, paths
        assert sum('ethereum:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48' in path for path in fallback) >= 2, paths
        print('Quota-independent proactive refresh passed.')
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=2)
