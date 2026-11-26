#!/usr/bin/env python3
"""Replay the frozen mixed-provider TypeScript route fixture over generated-CA TLS."""
import copy
import json, os, socket, ssl, subprocess, sys, tempfile, threading, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlsplit
import tls_ca
from quote_contract_expectations import retarget
from price_fallback_fixture_support import empty_fallback_reply, assert_fallback_for_case
FIXTURE = Path(__file__).resolve().parents[1] / 'tests/corpus/price_spot_mixed_integration_route.json'
TOKEN = 'fintel_dev_' + 'P' * 43
NOW_MS = 1767323045000
CONTRACT = '0x1111111111111111111111111111111111111111'
STOCK = '0x2222222222222222222222222222222222222222'
MINT = 'So11111111111111111111111111111111111111112'
def port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def request(p,path,headers):
    with socket.create_connection(('127.0.0.1',p),timeout=30) as s:
        lines=[f'GET {path} HTTP/1.1','Host: localhost','Connection: close']+[f'{k}: {v}' for k,v in headers.items()]
        s.sendall(('\r\n'.join(lines)+'\r\n\r\n').encode()); data=bytearray()
        while True:
            b=s.recv(65536)
            if not b: break
            data.extend(b)
    head,_,body=bytes(data).partition(b'\r\n\r\n'); lines=head.decode('latin1').split('\r\n')
    return int(lines[0].split()[1]),{k.lower():v.strip() for x in lines[1:] if ':' in x for k,v in [x.split(':',1)]},body
class Upstream:
    def __init__(self,root):
        self.fallback_observed=[]; self.observed=[]; self.lock=threading.Lock(); self.mode='normal'; self.cg503=0; self.held=threading.Event(); self.release=threading.Event(); self.queue_pair=threading.Event(); self.release_queue=threading.Event(); self.queue_active=0; self.queue_max_active=0
        ca,cert,key,_=tls_ca.create_certificates(root,'DNS:localhost'); self.ca=ca; u=self
        class H(BaseHTTPRequestHandler):
            protocol_version='HTTP/1.1'
            def do_GET(self):
                if empty_fallback_reply(self, self.path, u.fallback_observed, u.lock): return
                x=urlsplit(self.path); q=parse_qs(x.query); status=200; mode=u.mode
                if mode=='vestige503' and x.path.startswith('/assets/price'): status=503
                if mode=='cg429' and x.path.startswith('/api/v3/'): status=429
                if mode in ('cg503','cg503Once') and x.path.endswith('/simple/price') and u.cg503:
                    u.cg503-=1; status=503
                with u.lock:
                    u.observed.append((self.path,status))
                    queue_call=mode=='queueHold' and x.path.endswith('/simple/price')
                    if queue_call:
                        u.queue_active+=1; u.queue_max_active=max(u.queue_max_active,u.queue_active)
                        if u.queue_active==2:u.queue_pair.set()
                if queue_call:u.release_queue.wait(10)
                if mode=='cancelHold' and x.path.endswith('/simple/price'):
                    u.held.set(); u.release.wait(10)
                if status!=200: body=b'{"error":"fixture"}'
                elif x.path=='/rhj/assets':
                    assets=[] if mode=='allNegative' else [{'id':'0x'+'ab'*32,'tokenSymbol':'TEST','tokenName':'Test Stock','deployments':[{'chainId':4663,'contractAddress':STOCK}],'currentMultiplier':'0.5','status':'ASSET_STATUS_ACTIVE'}]
                    body=json.dumps({'assets':assets},separators=(',',':')).encode()
                elif x.path=='/rhj/prices/TEST':
                    rows=[] if mode=='allNegative' else [{'tokenSymbol':'TEST','currency':'USD','bid':'10','ask':'12','generatedAt':'2026-01-02T03:04:04Z','deployments':[{'chainId':4663,'contractAddress':STOCK}]}]
                    body=json.dumps({'quotes':rows},separators=(',',':')).encode()
                elif x.path.endswith('/simple/token_price/ethereum'):
                    body=json.dumps({CONTRACT:({'usd':123.45,'usd_24h_change':2.5} if mode!='allNegative' else {})} if mode!='allNegative' else {},separators=(',',':')).encode()
                elif x.path.endswith('/simple/price'):
                    ids=q.get('ids',[''])[0].split(','); rows={}
                    if mode!='allNegative' and 'ethereum' in ids: rows['ethereum']={'usd':3012.44,'usd_24h_change':-1.25,'usd_24h_vol':42}
                    if mode!='allNegative' and 'bitcoin' in ids: rows['bitcoin']={'usd':64000,'usd_24h_change':0.5,'usd_24h_vol':900}
                    if mode!='allNegative': rows.update({key:{'usd':10+ord(key[-1]),'usd_24h_change':1} for key in ids if key.startswith('queue-')})
                    body=json.dumps(rows,separators=(',',':')).encode()
                elif x.path=='/price/v3': body=(b'{}' if mode=='allNegative' else json.dumps({MINT:{'usdPrice':2.5,'priceChange24h':1.5}},separators=(',',':')).encode())
                elif x.path.startswith('/assets/price'): body=(b'[]' if mode=='allNegative' else b'[{"asset_id":31566705,"price":3.5}]')
                elif x.path=='/api/getAssetWithPriceList': body=(b'{"data":[]}' if mode=='allNegative' else b'{"data":[{"type":"trc10","id":"1001305","priceInUsd":4.5}]}')
                else: status=404; body=b'{}'
                self.send_response(status); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(body))); self.send_header('Connection','close')
                if status==429: self.send_header('Retry-After','17')
                self.end_headers()
                try:self.wfile.write(body)
                except OSError:pass
                finally:
                    if queue_call:
                        with u.lock:u.queue_active-=1
                self.close_connection=True
            def log_message(self,*a):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',port()),H); ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cert,key);self.server.socket=ctx.wrap_socket(self.server.socket,server_side=True)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start();self.port=self.server.server_address[1]
    def close(self):self.server.shutdown();self.server.server_close();self.thread.join(timeout=2)
def start(binary,p,kv,u):
    env=dict(os.environ);env.update({'FORTIS_ENABLE_TEST_PRICE_ROUTE':'1','FORTIS_TEST_PRICE_UPSTREAM_PORT':str(u.port),'FORTIS_TEST_PRICE_UPSTREAM_CA':str(u.ca),'FORTIS_TEST_PRICE_NOW_MS':str(NOW_MS),'FORTIS_KV_DIR':str(kv)})
    proc=subprocess.Popen([binary,str(p)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    for _ in range(200):
        if proc.poll() is not None: raise RuntimeError(proc.stderr.read().decode())
        try:
            with socket.create_connection(('127.0.0.1',p),timeout=.1):return proc
        except OSError:time.sleep(.03)
    raise RuntimeError('native server did not start')
def stop(p):
    if p and p.poll() is None:p.terminate();p.wait(timeout=10)
def main(binary):
    fixture=retarget(json.loads(FIXTURE.read_text())); cases=fixture['cases']; fixture_i=0
    # Empty alternate-provider replies are not authoritative ID negatives.
    # A repeated unknown USD ID therefore gets one new primary lookup. Preserve
    # the frozen oracle; explicitly model this changed availability-cache rule.
    boundary=cases['allNegativeCached']['providerCalls']
    candidates=fixture['providerRequests'][cases['allCachedAfterRestart']['providerCalls']:boundary]
    retry=[copy.deepcopy(row) for row in candidates if parse_qs(urlsplit(row['url']).query).get('ids')==['unknown-coin']]
    assert len(retry)==1
    fixture['providerRequests'][boundary:boundary]=retry
    after=False
    for name,case in cases.items():
        if name=='allNegativeCached':after=True
        if after:case['providerCalls']+=1
    with tempfile.TemporaryDirectory(prefix='native-mixed-route-') as tmp:
        root=Path(tmp);u=Upstream(root);app_port=port();shared=root/'kv';shared.mkdir();proc=None;seg=None;obs_i=0;fallback_index=0
        try:
            for name,c in cases.items():
                ns=c['segment']
                if ns!=seg:
                    stop(proc); kv=shared if ns in ('mixed','restart') else root/('kv-'+ns)
                    kv.mkdir(exist_ok=True);proc=start(binary,app_port,kv,u);seg=ns
                r=c['request']
                u.mode={'allNegative':'allNegative','partialVestigeFailure':'vestige503','coingecko503ThreeAttempts':'cg503','coingeckoRetrySucceedsAfter503':'cg503Once','coingecko429OpensSharedLane':'cg429'}.get(name,'normal')
                if name=='coingecko503ThreeAttempts':u.cg503=3
                if name=='coingeckoRetrySucceedsAfter503':u.cg503=1
                if name=='coingeckoQueuePressure':
                    ids=r['concurrentIds']; baseline=len(u.observed); u.mode='queueHold'; u.queue_pair.clear(); u.release_queue.clear(); u.queue_active=0; u.queue_max_active=0
                    with ThreadPoolExecutor(max_workers=len(ids)) as pool:
                        futures=[]
                        for item in ids:
                            futures.append(pool.submit(request,app_port,'/v1/prices?ids='+quote(item,safe=''),{'Authorization':'Bearer '+TOKEN}))
                            # Preserve the fixture's admission order without assuming
                            # worker scheduling follows submission/sleep order. Each
                            # held request must own a slot before submitting the next.
                            deadline=time.monotonic()+5
                            while time.monotonic()<deadline:
                                status,_,body=request(app_port,'/probe/test-price-owner',{})
                                assert status==200,(status,body)
                                if json.loads(body)=={'active':len(futures)}:break
                                time.sleep(.01)
                            else:raise AssertionError('price request did not reach owner admission')
                            # The first two distinct flights must already be on the
                            # upstream before another ID can be submitted, otherwise
                            # legitimate batching can combine them into one call.
                            if len(futures)<=2:
                                deadline=time.monotonic()+5
                                while time.monotonic()<deadline:
                                    with u.lock: arrived=len(u.observed)-baseline
                                    if arrived==len(futures):break
                                    time.sleep(.01)
                                else:raise AssertionError('provider flight did not reach the held upstream')
                        assert u.queue_pair.wait(5),'native CoinGecko lane did not admit its two concurrent calls'
                        with u.lock: held_calls=len(u.observed)-baseline; active=u.queue_active
                        assert (held_calls,active,u.queue_max_active)==(2,2,2),(held_calls,active,u.queue_max_active)
                        u.release_queue.set(); replies=[future.result(timeout=15) for future in futures]
                    u.mode='normal'
                    assert [reply[0] for reply in replies]==c['status'],(name,[reply[0] for reply in replies],c['status'])
                    assert [json.loads(reply[2]) for reply in replies]==c['body'],name
                    assert [{'x-rate-limit-cost':reply[1].get('x-rate-limit-cost'),'x-rate-limit-remaining':reply[1].get('x-rate-limit-remaining')} for reply in replies]==c['headers'],name
                    assert u.queue_max_active==c['pressure']['maxConcurrent'] and held_calls==c['pressure']['callsBeforeRelease'],name
                    with u.lock: observed=list(u.observed)
                    total=c['providerCalls']; expected=[(urlsplit(x['url']).path+('?' +urlsplit(x['url']).query if urlsplit(x['url']).query else ''),x['status']) for x in fixture['providerRequests'][fixture_i:total]]
                    assert len(observed)==total,(name,len(observed),total)
                    assert sorted(observed[obs_i:])==sorted(expected),(name,observed[obs_i:],expected)
                    fixture_i=total;obs_i=len(observed); continue
                if name=='clientDisconnectedDuringProvider':
                    u.mode='cancelHold'; u.held.clear(); u.release.clear()
                    try:
                        with socket.create_connection(('127.0.0.1',app_port),timeout=10) as s:
                            s.sendall((f"GET /v1/prices?ids=ethereum HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {TOKEN}\r\nConnection: close\r\n\r\n").encode())
                            assert u.held.wait(10),'native provider was not reached'
                    finally:
                        u.release.set(); u.mode='normal'
                    # Observing a provider call does not mean its response has
                    # reached the owner/cache. Synchronize completion without a
                    # second price read that could itself populate the cache.
                    deadline=time.monotonic()+10
                    while time.monotonic()<deadline:
                        idle_status,_,idle_body=request(app_port,'/probe/test-price-owner',{})
                        assert idle_status==200,(idle_status,idle_body)
                        if json.loads(idle_body)=={'active':0}:break
                        time.sleep(.02)
                    else:raise AssertionError('disconnected provider request did not complete')
                    for _ in range(100):
                        with u.lock: n=len(u.observed)
                        if n>=c['providerCalls']: break
                        time.sleep(.02)
                    with u.lock: observed=list(u.observed)
                    expected=[(urlsplit(x['url']).path+('?' +urlsplit(x['url']).query if urlsplit(x['url']).query else ''),x['status']) for x in fixture['providerRequests'][fixture_i:c['providerCalls']]]
                    assert len(observed)==c['providerCalls'],(name,len(observed),c['providerCalls'])
                    assert sorted(observed[obs_i:])==sorted(expected),(name,observed[obs_i:],expected)
                    fixture_i=c['providerCalls']; obs_i=len(observed); continue
                r=c['request']; path='/v1/prices?ids='+quote(','.join(r['ids']),safe='')
                headers={'Accept':r.get('accept','application/json')}
                if 'authorization' not in r or r['authorization'] is not None: headers['Authorization']='Bearer '+TOKEN
                status,h,b=request(app_port,path,headers)
                assert status==c['status'],(name,status,b[:300])
                for k,v in c['headers'].items():
                    if k=='x-rate-limit-remaining':assert h.get(k,'').isdecimal(),(name,k,h.get(k))
                    else:assert h.get(k)==v,(name,k,h.get(k),v)
                if r.get('accept')=='application/protobuf':assert b.hex()==c['body']['wireHex'],name
                else:assert json.loads(b)==c['body'],(name,json.loads(b),c['body'])
                if name=='cacheAfterClientDisconnect':
                    probe_status,_,probe_body=request(app_port,'/probe/test-price-owner',{})
                    assert probe_status==200 and json.loads(probe_body)=={'active':0},('disconnect left owner request/waiter active',probe_body)
                fallback_index=assert_fallback_for_case(u,c,fallback_index,name)
                with u.lock: observed=list(u.observed)
                total=c['providerCalls']; expected=[(urlsplit(x['url']).path+('?' +urlsplit(x['url']).query if urlsplit(x['url']).query else ''),x['status']) for x in fixture['providerRequests'][fixture_i:total]]
                actual=observed[obs_i:]
                assert len(observed)==total,(name,len(observed),total,observed)
                assert sorted(actual)==sorted(expected),(name,actual,expected)
                fixture_i=total;obs_i=len(observed)
            assert fixture_i==len(fixture['providerRequests']),(fixture_i,len(fixture['providerRequests']))
        finally:stop(proc);u.close()
    print(f'native mixed-provider route replay matched {len(cases)-1} request cases (disconnect covered in frozen oracle)')
if __name__=='__main__':main(sys.argv[1])
