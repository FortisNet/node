#!/usr/bin/env python3
"""Exercise two coordinator leases against a generated-CA HTTPS server."""
import json, socket, ssl, subprocess, sys, tempfile, threading, time
from pathlib import Path
import tls_ca

binary = Path(sys.argv[1]).resolve()
root = Path(tempfile.mkdtemp(prefix='provider-coordinator-'))
ca, cert, key, _ = tls_ca.create_certificates(root, 'DNS:localhost')
port = tls_ca.free_port()
marker = root / 'two-active'
stats = root / 'stats.json'
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
lock = threading.Lock()
active = 0
maximum = 0
seen = []

def handle(client):
    global active, maximum
    try:
        with context.wrap_socket(client, server_side=True) as stream:
            data = b''
            while b'\r\n\r\n' not in data:
                part = stream.recv(4096)
                if not part:
                    return
                data += part
            path = data.split(b' ', 2)[1].decode()
            with lock:
                active += 1
                maximum = max(maximum, active)
                seen.append(path)
                if path in ('/cancel-a', '/peer-b') and active == 2:
                    marker.touch()
            time.sleep(0.8)
            payload = path.encode()
            status = b'503 Unavailable' if path == '/keyless-503' else b'429 Too Many Requests' if path == '/limited-429' else b'429 Too Many Requests' if 'quota=true' in path else b'404 Not Found' if path.startswith('/api/v3/coins/') else b'200 OK'
            stream.sendall(b'HTTP/1.1 ' + status + b'\r\nContent-Type: text/plain\r\nContent-Length: '
                           + str(len(payload)).encode() + b'\r\nConnection: close\r\n\r\n' + payload)
            with lock:
                stats.write_text(json.dumps({'max_active': maximum, 'seen': seen}))
            with lock:
                active -= 1
    except (OSError, ssl.SSLError):
        pass

class Server(threading.Thread):
    daemon = True
    def run(self):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', port)); listener.listen()
        self.listener = listener
        while True:
            try:
                client, _ = listener.accept()
            except OSError:
                return
            threading.Thread(target=handle, args=(client,), daemon=True).start()

server = Server(); server.start()
try:
    deadline = time.monotonic() + 5
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), .1):
                break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.02)
    result = subprocess.run([str(binary), str(port), str(ca), str(marker)],
                            capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise SystemExit(f'coordinator test exited {result.returncode}\n{result.stdout}\n{result.stderr}')
    observed = json.loads(stats.read_text())
    assert observed['max_active'] == 2, observed
    assert sorted(observed['seen']) == sorted(['/cancel-a', '/keyless-503', '/limited-429', '/owner-a', '/owner-b', '/peer-b', '/api/v3/coins/obsolete-coin?localization=false', '/api/v3/coins/list', '/api/v3/coins/ethereum/contract/0x123', '/api/v3/coins/quota-coin?quota=true']), observed
    print(result.stdout.strip())
    print('shared CoinGecko lane held TLS concurrency to two')
finally:
    if hasattr(server, 'listener'):
        server.listener.close()
    import shutil
    shutil.rmtree(root, ignore_errors=True)
