"""Reviewed contract changes over SHA-verified, immutable TypeScript inputs."""
import copy
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

FIXTURES = Path(__file__).parent / 'fixtures'
SUITES = {'cross_chain_quotes_edges.json':'quotes', 'cross_chain_status_edges.json':'status',
          'robinhood_prepare_edges.json':'R43', 'robinhood_submit_edges.json':'R44',
          'robinhood_diagnose_edges.json':'R45', 'bittensor_analytics_edges.json':'bittensor',
          'price_markets_edges.json':'markets'}


def apply(name, fixture):
    length_changes = {}
    for path, value in json.loads((FIXTURES/'strict_contract_cases.json').read_text()).get(name, []):
        target = fixture
        for key in path[:-1]: target = target[key]
        if path[-1] in ('responseText','wireHex'):
            old = target[path[-1]]
            old_size = len(bytes.fromhex(old)) if path[-1]=='wireHex' else len(old.encode())
            new_size = len(bytes.fromhex(value)) if path[-1]=='wireHex' else len(value.encode())
            if old_size != new_size: length_changes[str(old_size)] = str(new_size)
        target[path[-1]] = value
        if path[-1] in ('responseText','wireHex') and isinstance(target.get('headers'),dict):
            target['headers']['content-length'] = str(len(bytes.fromhex(value)) if path[-1]=='wireHex' else len(value.encode()))
    def head_lengths(value):
        if isinstance(value,dict):
            if value.get('status')==200 and value.get('responseText')=='' and value.get('headers',{}).get('content-length') in length_changes:
                value['headers']['content-length'] = length_changes[value['headers']['content-length']]
            for child in value.values(): head_lengths(child)
        elif isinstance(value,list):
            for child in value: head_lengths(child)
    head_lengths(fixture)
    if name=='sync_route.json':
        strict = {'limitString','limitStringSpaces','limitHexString','limitTrue','limitArray','limitStringExponent','limitStringNewline','cursorNumber','cursorBoolean','cursorNull','cursorFraction','cursorArray','cursorLargeNumber','afterArray','modelsScalar','modelsNestedArray'}
        for row in fixture['cases']:
            if row['name'] in strict:
                row.update(status=400,body={'error':'BAD_REQUEST'},bodyText='{"error":"BAD_REQUEST"}')
                row['headers']={k:v for k,v in row['headers'].items() if k not in ('vary','etag')}
                row['headers'].update({'cache-control':'no-store','content-length':'23'})
    if name=='token_history_edges.json':
        import re
        rows=fixture['cases']; by_name={r['name']:r for r in rows}
        refusals={'holders-days-padded','holders-days-hex','holders-days-exponent','holders-days-plus','holders-days-tab','holders-days-long-padded','holders-days-plus-space','holders-days-other-key','holders-sources-bad-escape','holders-query-other-keys','history-unknown-keys'}
        pairs={'holders-days-repeated':'holders-series-days-7','holders-source-repeated':'holders-sources-bad-escape','history-range-repeated':'history-fresh-1d-1h','history-interval-repeated':'history-fresh-default'}
        # Snapshot source twins before their old coercion cases are retargeted.
        twins={key:copy.deepcopy(by_name[value]) for key,value in pairs.items()}
        for row in rows:
            if row['name'] in refusals:
                row.update(status=400,responseText='{"error":"BAD_REQUEST"}',upstream=[])
                row['headers']={k:v for k,v in row['headers'].items() if k not in ('vary','etag')}
                row['headers'].update({'cache-control':'no-store','content-length':'23'})
            if row['name'] in twins:
                source=twins[row['name']];text=source['responseText']
                if row['name']=='holders-source-repeated': text=text.replace('"trendSource":"%zz"','"trendSource":"a"')
                if row['name'].startswith('history-'):
                    delta=row['at']//1000-source['at']//1000
                    text=re.sub(r'"latestAgeSeconds":(\d+)',lambda m:'"latestAgeSeconds":'+str(int(m[1])+delta),text)
                    text=re.sub(r'"(from|to)":(\d+)',lambda m:'"'+m[1]+'":'+str(int(m[2])+delta),text)
                row.update(status=200,responseText=text,headers=copy.deepcopy(source['headers']),upstream=[])
                row['headers']['content-length']=str(len(text.encode()))
                prefix='token-liquidity-history' if row['name'].startswith('history-') else 'token-holder-history'
                if not row['name'].startswith('history-'): row['headers']['etag']='W/"'+prefix+'-'+hashlib.sha256(text.encode()).hexdigest()[:24]+'"'
    if name=='token_search_edges.json':
        cases=fixture['cases']
        bad=next(r for r in cases if r.get('status')==400 and r.get('responseText')=='{"error":"BAD_REQUEST"}' and 'x-rate-limit-cost' in r.get('headers',{}))
        unavailable=next(r for r in cases if r.get('status')==502 and r.get('responseText')=='{"error":"UPSTREAM_UNAVAILABLE"}')
        for row in cases:
            if row['name'] in ('term-bad-escape','term-invalid-utf8','term-lone-surrogate'):
                row.update(status=400,responseText=bad['responseText'],headers=copy.deepcopy(bad['headers']),upstream=[])
            if row['name'] in ('q-repeated','q-repeated-same','q-repeated-empty'):
                row.update(status=502,responseText=unavailable['responseText'],headers=copy.deepcopy(unavailable['headers']))
                term='a' if row['name']=='q-repeated' else 'usd'
                row['upstream']=[] if row['name']=='q-repeated-empty' else [dict(method='GET',url='https://api.coingecko.com/api/v3/search?query='+term,status=599,body='unexpected request',headers={'accept':'application/json'}) for _ in range(3)]
    if name=='robinhood_instrument_edges.json':
        for row in fixture['cases']:
            if row['name']=='lone-surrogate-symbol':
                row['upstream']=[{'method':'GET','url':'https://api.robinhood.com/rhj/prices/%EF%BF%BDX','headers':{'accept':'application/json'}}]
    if name=='price_markets_edges.json':
        row=next(r for r in fixture['cases'] if r['name']=='lone-surrogate-id')
        row['responseText']='{"markets":[],"pending":[],"cache":{"hit":false,"timestamp":null}}'
        row['headers']['content-length']=str(len(row['responseText']))
        row['upstream']=[{'status':200,'body':'[]','method':'GET','url':'https://api.coingecko.com/api/v3/coins/markets?vs_currency=usd&ids=alpha%EF%BF%BD&price_change_percentage=1h,24h,7d,14d,30d,200d,1y&sparkline=true','headers':{'accept':'application/json'}}]
    suite = SUITES.get(name)
    if not suite: return fixture
    rejected = json.loads((FIXTURES/'strict_proto_cases.json').read_text())[suite]
    cases = fixture['cases']
    template = next(r for r in cases if r.get('status') == 400 and r.get('responseText') == '{"error":"BAD_REQUEST"}' and 'x-rate-limit-cost' in r.get('headers',{}))
    def refuse(row, detail=None):
        cost = row['headers'].get('x-rate-limit-cost')
        row['status'] = 400
        row['responseText'] = json.dumps({'error':'BAD_REQUEST', **({'detail':detail} if detail else {})},separators=(',',':'))
        row['headers'] = copy.deepcopy(template['headers'])
        if cost is not None: row['headers']['x-rate-limit-cost'] = cost
        row['headers']['content-length'] = str(len(row['responseText'].encode()))
        for key in ('wireHex','protoOfJson','decoded'): row.pop(key,None)
        for key in ('upstream','kvReads','kvWrites'):
            if key in row: row[key] = []
        row['strict_expected_change'] = 'strict protobuf request validation'
    for row in cases:
        if row['name'] in rejected: refuse(row)
        if suite != 'bittensor': continue
        for key in ('wireHex','protoOfJson'):
            if row.get(key): row[key] = _message(bytes.fromhex(row[key]), 'response').hex()
        if row['name'] == 'pb-path-bom': refuse(row,'unsupported bittensor analytics path')
        if row['name'] == 'pb-key-bom': refuse(row,'unsupported bittensor query')
        if row['name'] == 'pb-value-bom':
            row['upstream'][0]['url'] = 'https://api.taostats.io/api/subnet/owner/v1?owner=%EF%BB%BFpb-bom-value'
            key = 'taostats:' + hashlib.sha256(('/api/subnet/owner/v1:' + json.dumps([['owner','\ufeffpb-bom-value']],ensure_ascii=False,separators=(',',':'))).encode()).hexdigest()[:24]
            row['kvReads'] = [key]
            for write in row['kvWrites']: write['key'] = key
        if row['name'] == 'pb-query-unknown-entry-field':
            # The map-entry unknown varint is legal. The same account was
            # cached earlier; construct this exact read from its source case.
            source = next(r for r in cases if r['name']=='path-account')
            row['status'] = 200
            row['headers'] = copy.deepcopy(source['headers'])
            previous = [write for prior in cases[:cases.index(row)] if prior['group']==row['group'] for write in prior.get('kvWrites',[]) if write['key']==source['kvReads'][0]]
            stored = previous[-1]
            timestamp = datetime.fromtimestamp(stored['asOf']/1000,timezone.utc).isoformat(timespec='milliseconds').replace('+00:00','Z')
            row['responseText'] = '{"payload":'+stored['d']+',"cache":{"hit":true,"timestamp":'+json.dumps(timestamp)+'}}'
            row['headers']['content-length'] = str(len(row['responseText'].encode()))
            row['kvReads'] = source['kvReads'][:]
    if suite in ('R43','R45'):
        add = {'R43':{'pb-sponsored-unknown','policy-swap-only','policy-v3-sentinel-1','policy-two-swaps','alchemy-invalid-sender','alchemy-http-500'},
               'R45':{'pb-unknown-group','sim-trace-null','sim-http-500'}}[suite]
        remove = {'R43':{'policy-v3-sentinel-unwrap','policy-selector-v3-bare','alchemy-invalid-chain-number','alchemy-missing-result'},
                  'R45':{'sim-block-not-record','sim-missing-result'}}[suite]
        sample=next(copy.deepcopy(call) for row in cases for call in row.get('upstream',[]) if call['url']=='https://api.robinhood.com/rhj/assets')
        for row in cases:
            if row['group']!='main': continue
            if row['name'] in remove: row['upstream']=[call for call in row['upstream'] if call['url']!=sample['url']]
            if row['name'] in add: row['upstream'].insert(0,copy.deepcopy(sample))
    return fixture


def _varint(value):
    result = bytearray()
    while value > 127: result.append((value & 127)|128); value >>= 7
    result.append(value)
    return bytes(result)


def _fields(data):
    position = 0
    def integer():
        nonlocal position
        value = shift = 0
        while True:
            byte = data[position]; position += 1
            value |= (byte & 127) << shift
            if byte < 128: return value
            shift += 7
            if shift > 63: raise ValueError('unbounded fixture varint')
    while position < len(data):
        start = position
        tag = integer(); wire = tag & 7
        if wire == 2:
            length = integer(); payload = data[position:position+length]; position += length
        elif wire == 0: integer(); payload = None
        elif wire in (1,5): position += 8 if wire == 1 else 4; payload = None
        else: raise ValueError('unsupported fixture wire')
        yield tag, payload, data[start:position]


def _message(data, kind):
    fields = list(_fields(data))
    if kind == 'struct':
        entries = []
        for tag,payload,raw in fields:
            assert tag == 10
            entry = list(_fields(payload))
            key = next(p.decode() for t,p,r in entry if t == 10)
            entries.append((key, payload))
        keys = [key for key,p in entries]
        if all(key in keys for key in ('1','2','10','address','rank')):
            rest = [(k,p) for k,p in entries if k not in ('1','2','10')]
            at = next(i for i,(k,p) in enumerate(rest) if k=='rank')+1
            rest[at:at] = [next(e for e in entries if e[0]==k) for k in ('2','1','10')]
            entries = rest
        fields = [(10,p,b'') for k,p in entries]
    result = bytearray()
    for tag,payload,raw in fields:
        child = {'response':{10:'value'},'value':{42:'struct',50:'list'},'list':{10:'value'},'struct':{10:'entry'},'entry':{18:'value'}}[kind].get(tag)
        if child and payload is not None:
            payload = _message(payload,child)
            result += _varint(tag)+_varint(len(payload))+payload
        else: result += raw
    return bytes(result)


def encode_structured(value):
    """Standard google.protobuf.Struct / Value framing, independent of C++."""
    import struct
    def ld(field,payload): return _varint(field*8+2)+_varint(len(payload))+payload
    def val(value):
        if value is None: return b'\x08\x00'
        if isinstance(value,bool): return b'\x20'+bytes([int(value)])
        if isinstance(value,(int,float)): return b'\x11'+struct.pack('<d',float(value))
        if isinstance(value,str): return ld(3,value.encode())
        if isinstance(value,list): return ld(6,b''.join(ld(1,val(x)) for x in value))
        return ld(5,st(value))
    def st(value):
        return b''.join(ld(1,ld(1,k.encode())+ld(2,val(v))) for k,v in value.items())
    return ld(1,st(value))
