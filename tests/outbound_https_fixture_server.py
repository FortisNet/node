#!/usr/bin/env python3
import socket, ssl, sys, time
port, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3]
ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cert,key)
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1',port));s.listen(16)
while True:
 c,_=s.accept()
 try:
  with ctx.wrap_socket(c,server_side=True) as x:
   data=b''
   while b'\r\n\r\n' not in data and len(data)<20000:
    part=x.recv(4096)
    if not part:break
    data+=part
   if not data:continue
   head,_,body=data.partition(b'\r\n\r\n'); path=head.split(b' ')[1]
   host_lines=[line.lower() for line in head.split(b'\r\n')[1:] if line.lower().startswith(b'host:')]
   if len(host_lines) != 1 or host_lines[0] not in (f'host: localhost:{port}'.encode(), f'host: 10.0.2.2:{port}'.encode()):
    x.sendall(b'HTTP/1.1 400 Bad Host\r\nContent-Length: 0\r\n\r\n');continue
   if path==b'/slow':time.sleep(0.35)
   if path==b'/chunked-ext':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n4;foo=bar;quoted=\"a b\"\r\ndata\r\n0\r\n\r\n');continue
   if path==b'/chunked':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: accepted\r\n\r\n');continue
   if path==b'/bad-chunk':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nZ\r\n');continue
   if path==b'/bad-chunk-crlf':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2\r\nokX\n');continue
   if path==b'/truncated-chunk':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabc');continue
   if path==b'/bad-trailer':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\nContent-Length: 0\r\n\r\n');continue
   if path==b'/oversized-trailer':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\nX-Fill: '+b'a'*17000+b'\r\n\r\n');continue
   if path==b'/chunked-plus-length':x.sendall(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n');continue
   if path==b'/eof':x.sendall(b'HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody');continue
   if path==b'/eof-keepalive':x.sendall(b'HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\nbody');continue
   if path==b'/truncated':x.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nabc');continue
   if path==b'/gzip':x.sendall(b'HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 2\r\n\r\nxx');continue
   if path==b'/duplicate-length':x.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok');continue
   if path==b'/conflicting-length':x.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n');continue
   if path==b'/continue':x.sendall(b'HTTP/1.1 100 Continue\r\n\r\n');continue
   if path==b'/no-content':x.sendall(b'HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n');continue
   if path==b'/oversized-header':x.sendall(b'HTTP/1.1 200 OK\r\nX-Fill: '+b'a'*17000+b'\r\nContent-Length: 0\r\n\r\n');continue
   if path==b'/oversized':x.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 65\r\n\r\n');continue
   if path==b'/metadata':status=b'429 Too Many Requests';response=b'busy';extra=b'Content-Type: application/json\r\nLocation: /next\r\nRetry-After: 7\r\n'
   elif path==b'/redirect':status=b'302 Found';response=b'';extra=b'Location: /next\r\n'
   elif path==b'/non2xx':status=b'503 Unavailable';response=b'no';extra=b'Retry-After: 2\r\n'
   elif path==b'/moderate':status=b'200 OK';response=b'x'*(256*1024);extra=b'Content-Type: application/octet-stream\r\n'
   elif path==b'/post':
    content=0
    for line in head.split(b'\r\n')[1:]:
     if line.lower().startswith(b'content-length:'):content=int(line.split(b':',1)[1])
    while len(body)<content:body+=x.recv(4096)
    status=b'200 OK';response=body[:content];extra=b'Content-Type: application/json\r\n'
   elif path in (b'/user-agent', b'/post-user-agent'):
    if path==b'/post-user-agent':
     content=0
     for line in head.split(b'\r\n')[1:]:
      if line.lower().startswith(b'content-length:'):content=int(line.split(b':',1)[1])
     while len(body)<content:body+=x.recv(4096)
    matches=[line for line in head.split(b'\r\n')[1:] if line.lower().startswith(b'user-agent:')]
    status=b'200 OK';response=matches[0] if len(matches)==1 else b'missing';extra=b'Content-Type: text/plain\r\n'
   elif path==b'/headers':
    matches=[line for line in head.split(b'\r\n')[1:] if line.startswith(b'X-Api-Key:')]
    status=b'200 OK';response=matches[0] if len(matches)==1 else b'missing';extra=b'Content-Type: text/plain\r\n'
   else:status=b'200 OK';response=b'ok';extra=b'Content-Type: text/plain\r\n'

   x.sendall(b'HTTP/1.1 '+status+b'\r\n'+extra+b'Content-Length: '+str(len(response)).encode()+b'\r\nConnection: close\r\n\r\n'+response)
 except (OSError,ssl.SSLError):pass
