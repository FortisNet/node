#!/usr/bin/env python3
import socketserver, ssl, sys, time, threading

port, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3]
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
counts = {}
counts_lock = threading.Lock()

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            with context.wrap_socket(self.request, server_side=True) as stream:
                data = b''
                while b'\r\n\r\n' not in data and len(data) < 20000:
                    part = stream.recv(4096)
                    if not part:
                        return
                    data += part
                head, _, body = data.partition(b'\r\n\r\n')
                method, path, _ = head.split(b' ', 2)
                if method == b'POST':
                    length = 0
                    for line in head.split(b'\r\n')[1:]:
                        if line.lower().startswith(b'content-length:'):
                            length = int(line.split(b':', 1)[1])
                    while len(body) < length:
                        body += stream.recv(4096)
                with counts_lock:
                    counts[path] = counts.get(path, 0) + 1
                    count = counts[path]
                if path.startswith(b'/api/v3/simple/price?'):
                    request_line = head.split(b'\r\n', 1)[0]
                    auth = next((line for line in head.split(b'\r\n')[1:]
                                 if line.lower().startswith(b'x-cg-pro-api-key:')),
                                b'missing-api-key')
                    status, payload, extra = b'200 OK', request_line + b'|' + auth, \
                                             b'Content-Type: text/plain\r\n'
                elif path == b'/slow':
                    time.sleep(0.35)
                elif path == b'/stall':
                    time.sleep(3)
                if path.startswith(b'/api/v3/simple/price?'):
                    pass
                elif path == b'/probeRetry' and count == 1:
                    status, payload, extra = b'503 Unavailable', b'probe retry', b''
                elif path in (b'/lease503', b'/retryShutdownLease', b'/retryDeadlineLease'):
                    status, payload, extra = b'503 Unavailable', b'lease retry', b''
                elif path in (b'/retry500', b'/retryDeadline', b'/retryHeader', b'/retryLease') and count == 1:
                    status, payload, extra = b'503 Unavailable', b'first', b'Retry-After: 1\r\n'
                elif path == b'/always503' or path == b'/post503':
                    status, payload, extra = b'503 Unavailable', b'failure', b'Retry-After: 0.1\r\n'
                elif path == b'/429':
                    status, payload, extra = b'429 Too Many Requests', b'busy', b'Retry-After: 7\r\n'
                elif path == b'/429Long':
                    status, payload, extra = b'429 Too Many Requests', b'busy', b'Retry-After: 180\r\n'
                elif path == b'/429Extreme':
                    status, payload, extra = b'429 Too Many Requests', b'busy', b'Retry-After: 86400\r\n'
                elif path == b'/retryHeader':
                    matches = [line for line in head.split(b'\r\n')[1:]
                               if line.startswith(b'X-Api-Key:')]
                    status = b'200 OK'
                    payload = matches[0] if len(matches) == 1 else b'missing'
                    extra = b'Content-Type: text/plain\r\n'
                else:
                    status, payload, extra = b'200 OK', b'ok', b'Content-Type: text/plain\r\n'
                stream.sendall(b'HTTP/1.1 ' + status + b'\r\n' + extra +
                               b'Content-Length: ' + str(len(payload)).encode() +
                               b'\r\nConnection: close\r\n\r\n' + payload)
        except (OSError, ssl.SSLError):
            pass

with Server(('127.0.0.1', port), Handler) as server:
    server.serve_forever()
