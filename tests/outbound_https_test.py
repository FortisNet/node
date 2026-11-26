#!/usr/bin/env python3
"""Deterministic TLS client coverage against generated local certificates."""
import os, socket, subprocess, sys, tempfile, time
from pathlib import Path
import tls_ca
create_certificates, free_port = tls_ca.create_certificates, tls_ca.free_port

def main():
 binary=Path(sys.argv[1]).resolve(); root=Path(tempfile.mkdtemp(prefix='outbound-http-'))
 ca,cert,key,other=tls_ca.create_certificates(root,'DNS:localhost'); port=tls_ca.free_port()
 server=subprocess.Popen([sys.executable,str(Path(__file__).with_name('outbound_https_fixture_server.py')),str(port),str(cert),str(key)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 try:
  for _ in range(100):
   try:
    with socket.create_connection(('127.0.0.1',port),.1):break
   except OSError:time.sleep(.02)
  def req(path,timeout=1000,host='localhost',cafile=ca,allow=True,method='GET',max_bytes=None,second=None,rotate=None,header=None,header_count=None):
   env=os.environ.copy()
   if allow:env['ALLOW_LOCAL']='1'
   else:env.pop('ALLOW_LOCAL',None)
   if method=='POST':env['TEST_METHOD']='POST'
   else:env.pop('TEST_METHOD',None)
   if max_bytes is not None:env['MAX_RESPONSE_BYTES']=str(max_bytes)
   else:env.pop('MAX_RESPONSE_BYTES',None)
   if second:env['SECOND_PATH']=second
   else:env.pop('SECOND_PATH',None)
   if rotate:env['ROTATE_CA_TO']=str(rotate)
   else:env.pop('ROTATE_CA_TO',None)
   if header:
    pairs=header if isinstance(header[0],tuple) else (header,)
    first=pairs[0];second=pairs[1:]
    env['TEST_HEADER_NAME'],env['TEST_HEADER_VALUE']=first
    if second:env['TEST_SECOND_HEADER_NAME'],env['TEST_SECOND_HEADER_VALUE']=second[0]
    else:env.pop('TEST_SECOND_HEADER_NAME',None);env.pop('TEST_SECOND_HEADER_VALUE',None)
   else:
    env.pop('TEST_HEADER_NAME',None);env.pop('TEST_HEADER_VALUE',None)
    env.pop('TEST_SECOND_HEADER_NAME',None);env.pop('TEST_SECOND_HEADER_VALUE',None)
   if header_count is None:env.pop('TEST_HEADER_COUNT',None)
   else:env['TEST_HEADER_COUNT']=str(header_count)
   proc=subprocess.run([str(binary),host,str(port),path,str(cafile),str(timeout)],capture_output=True,text=True,env=env,timeout=6)
   lines=proc.stdout.strip().splitlines()
   def parse(line):
    first,content_type,location,retry_after,payload=line.split('|',4)
    code,status,length=first.split()
    return int(code),int(status),int(length),content_type,location,retry_after,payload
   return [parse(line) for line in lines]
  result=req('/')[0];assert result[:3]==(0,200,2) and result[3]=='text/plain',result
  result=req('/post',method='POST')[0];assert result[:3]==(0,200,17) and result[6]=='{"hello":"world"}',result
  result=req('/headers',header=('X-Api-Key','fixture-token-42'))[0]
  assert result[:3]==(0,200,len('X-Api-Key: fixture-token-42')) and result[6]=='X-Api-Key: fixture-token-42'
  for method,path in (('GET','/user-agent'),('POST','/post-user-agent')):
   result=req(path,method=method)[0]
   assert result[0:2]==(0,200) and result[6]=='User-Agent: FortisNetWallet/1.0',(method,result)
  # Invalid custom headers fail before CA/DNS/socket activity and never include
  # their values in the response or diagnostic output.
  for bad in (('Host','attacker.example'),('Content-Length','0'),
              ('Connection','keep-alive'),('X-Bad','line\r\nbreak'),
              ('X-Bad','tab\tvalue'),('X Bad','value'),
              ('X-Bad','v'*256),('A'*64,'value')):
   result=req('/',header=bad)[0]
   assert result[0]==1
  assert req('/',header=(('X-Token','one'),('x-token','two')))[0][0]==1
  assert req('/',header_count=9)[0][0]==1
  result=req('/chunked')[0];assert result[:3]==(0,200,11) and result[6]=='hello world' and result[3]=='text/plain',result
  result=req('/chunked-ext')[0];assert result[:3]==(0,200,4) and result[6]=='data',result
  result=req('/eof')[0];assert result[:3]==(0,200,4) and result[6]=='body',result
  result=req('/duplicate-length')[0];assert result[:3]==(0,200,2),result
  result=req('/no-content')[0];assert result[:3]==(0,204,0),result
  result=req('/metadata')[0];assert result[:3]==(0,429,4) and result[3]=='application/json' and result[4]=='/next' and result[5]=='7',result
  result=req('/redirect')[0];assert result[:2]==(0,302) and result[4]=='/next',result
  result=req('/non2xx')[0];assert result[:2]==(0,503) and result[5]=='2',result
  for path, expected in (('/bad-chunk',7),('/bad-chunk-crlf',7),('/truncated-chunk',7),
                         ('/bad-trailer',7),('/chunked-plus-length',9),('/conflicting-length',9),
                         ('/eof-keepalive',9),('/truncated',7),('/gzip',9),
                         ('/continue',9),('/oversized-header',8),('/oversized-trailer',8),('/oversized',8)):
   result=req(path)[0];assert result[0]==expected,(path,result)
  result=req('/slow',timeout=120)[0];assert result[0]==4,result
  assert req('/',timeout=60001)[0][0]==1
  assert req('/',host='127.0.0.1')[0][0]==6
  assert req('/',cafile=other)[0][0]==6
  assert req('/',allow=False)[0][0]==2
  # Two calls in one process exercise immutable trust-context reuse. Replacing
  # the path does not mutate the published chain; loading the new CA requires restart.
  trusted_ca=ca.read_bytes()
  first,second=req('/',second='/',rotate=other)[0:2]
  assert first[0:2]==(0,200) and second[0:2]==(0,200),(first,second)
  ca.write_bytes(trusted_ca)
  started=time.monotonic()
  result=req('/moderate',timeout=5000,max_bytes=1024*1024,second='/moderate')
  elapsed=time.monotonic()-started
  assert len(result)==2 and all(item[0:3]==(0,200,256*1024) for item in result),result
  assert elapsed<5,elapsed
  print(f'outbound HTTPS framing, metadata, CA cache, limits, and buffered response passed (2 x 256 KiB in {elapsed*1000:.1f} ms)')
  # Plaintext mode (media http: URLs, feed redirects): same framing and
  # bounds without TLS or a CA file; the Host header names the port when it
  # is not 80. The all-addresses rule refuses before any connection.
  import http.server,threading
  seen=[]
  class Plain(http.server.BaseHTTPRequestHandler):
   protocol_version='HTTP/1.1'
   def do_GET(self):
    seen.append(dict(self.headers))
    body=b'plain-ok'
    self.send_response(200);self.send_header('Content-Type','text/plain');self.send_header('Content-Length',str(len(body)));self.send_header('Connection','close');self.end_headers();self.wfile.write(body)
   def log_message(self,*a):pass
  plain=http.server.ThreadingHTTPServer(('127.0.0.1',0),Plain);plain_port=plain.server_address[1]
  threading.Thread(target=plain.serve_forever,daemon=True).start()
  try:
   def plain_req(extra,cafile='-'):
    env=os.environ.copy();env.update({'ALLOW_LOCAL':'1','PLAINTEXT':'1'});env.update(extra)
    proc=subprocess.run([str(binary),'localhost',str(plain_port),'/x',cafile,'1000'],capture_output=True,text=True,env=env,timeout=6)
    first=proc.stdout.strip().split('|',1)[0].split()
    return int(first[0]),int(first[1]),proc.stdout
   assert plain_req({})[:2]==(0,200),plain_req({})
   assert seen[-1].get('Host')==f'localhost:{plain_port}',seen[-1]
   count=len(seen)
   result=plain_req({'REQUIRE_ALL':'1','DENY_ALL':'1'})
   assert result[:2]==(2,0) and len(seen)==count,result  # kPrivateAddress, never dialled
   # A given dial address skips resolution; the name still fills the Host
   # header and the address policy still judges the address.
   def dialled(extra):
    env=os.environ.copy();env.update({'PLAINTEXT':'1','CONNECT_LOOPBACK':'1'});env.update(extra)
    proc=subprocess.run([str(binary),'node.invalid',str(plain_port),'/x','-','1000'],capture_output=True,text=True,env=env,timeout=6)
    first=proc.stdout.strip().split('|',1)[0].split()
    return int(first[0]),int(first[1])
   assert dialled({'ALLOW_LOCAL':'1'})==(0,200)
   assert seen[-1].get('Host')==f'node.invalid:{plain_port}',seen[-1]
   count=len(seen)
   assert dialled({})==(2,0) and len(seen)==count  # loopback is not public
   # TLS still requires a CA file.
   env=os.environ.copy();env['ALLOW_LOCAL']='1';env.pop('PLAINTEXT',None)
   proc=subprocess.run([str(binary),'localhost',str(port),'/','-','1000'],capture_output=True,text=True,env=env,timeout=6)
   fields=proc.stdout.split()
   assert fields[0]!='0' and fields[1]=='0',proc.stdout
  finally:
   plain.shutdown();plain.server_close()
  print('outbound plaintext mode and all-addresses refusal passed')
 finally:
  server.terminate();server.wait(timeout=3)
  import shutil;shutil.rmtree(root,ignore_errors=True)

# Reuse the existing generated-CA fixture helpers without importing tests as a package.
if __name__=='__main__':main()
