#!/usr/bin/env python3
"""Independent, explicit expectation overlays; never reads server output."""
import copy
import json
import re
import sys
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3]/"tests"))
from strict_contract_expectations import _message, _fields, _varint, encode_structured
from generate_strict_text_cases import ROOT, json_strings, drop_lone

ORDER_OLD = '"1":"one","2":"two","10":"ten","address":'
ORDER_NEW = '"address":'
# Preserve existing number spellings and scalar encodings while restoring
# the documented source position of these three provider keys.
def source_order(text):
    if ORDER_OLD not in text: return text
    text = text.replace(ORDER_OLD, ORDER_NEW)
    return text.replace('"rank":3,"stake":', '"rank":3,"2":"two","1":"one","10":"ten","stake":')


class Number(str): pass

def parse(text):
    return json.loads(text, parse_int=Number, parse_float=Number)

def dump(value):
    if isinstance(value, Number): return str(value)
    if isinstance(value,dict): return '{'+','.join(json.dumps(k,ensure_ascii=False)+':'+dump(v) for k,v in value.items())+'}'
    if isinstance(value,list): return '['+','.join(map(dump,value))+']'
    return json.dumps(value,ensure_ascii=False,separators=(',',':'))

def order_like(value,source):
    if isinstance(value,list) and isinstance(source,list): return [order_like(v,source[i]) if i<len(source) else v for i,v in enumerate(value)]
    if isinstance(value,dict) and isinstance(source,dict):
        return {k:order_like(value[k],source[k]) for k in source if k in value} | {k:v for k,v in value.items() if k not in source}
    return value

def main():
    names = ['cross_chain_quotes_edges','cross_chain_status_edges','robinhood_instrument_edges','robinhood_stock_catalog_edges','robinhood_prepare_edges',
             'robinhood_submit_edges','robinhood_diagnose_edges','price_markets_edges','token_search_edges',
             'bittensor_analytics_edges','transaction_edges','news_reads_edges']
    overlays = {}
    for name in names:
        fixture = json.loads((ROOT/f'tests/corpus/{name}.json').read_text())
        patches = []
        def visit(value, path=(), row_name=''):
            if isinstance(value, dict):
                row_name = value.get('name', row_name)
                for key, child in value.items():
                    # Only expected output strings, never request/provider bytes.
                    selected = (key=='body' and 'upstream' in path and name in ('cross_chain_quotes_edges','cross_chain_status_edges','robinhood_submit_edges')) or (name == 'news_reads_edges' and key == 'value' and 'pricesValue' in path) or key in ('responseText','output','d','result') or (key == 'text' and 'json' in path)
                    if selected and isinstance(child, str):
                        updated = json_strings(child)
                        if name == 'bittensor_analytics_edges': updated = source_order(updated)
                        if name == 'token_search_edges' and row_name in ('coingecko-usd-b','coingecko-dup-order','coingecko-base-dup'):
                            # Sanitized names/symbols stop before the split emoji.
                            updated = re.sub(r'\\u[dD][89aAbB][0-9a-fA-F]{2}(?=…|")', '', child)
                            updated = json_strings(updated)
                        if name == 'robinhood_diagnose_edges' and row_name in ('sim-revert-long-reason','sim-rpc-error-message-emoji'):
                            updated = re.sub(r'\\u[dD][89aAbB][0-9a-fA-F]{2}(?=…|")', '', child)
                            updated = json_strings(updated)
                        if updated != child: patches.append([list(path+(key,)), updated])
                    else: visit(child, path+(key,), row_name)
            elif isinstance(value,list):
                for i, child in enumerate(value): visit(child,path+(i,),row_name)
        visit(fixture)
        for i,row in enumerate(fixture.get('cases', [])):
            if name == 'bittensor_analytics_edges':
                for key in ('wireHex','protoOfJson'):
                    if row.get(key):
                        old=bytes.fromhex(row[key]); new=_message(old,'response')
                        if new != old: patches.append([['cases',i,key],new.hex()])
            if name == 'token_search_edges' and row['name']=='coingecko-dup-order-protobuf':
                result=bytearray()
                for tag,payload,raw in _fields(bytes.fromhex(row['wireHex'])):
                    if tag==10:
                        item=bytearray()
                        for t,p,r in _fields(payload):
                            if t in (34,42):
                                p=p.replace('�…'.encode(),'…'.encode())
                                item+=_varint(t)+_varint(len(p))+p
                            else: item+=r
                        result+=_varint(tag)+_varint(len(item))+item
                    else: result+=raw
                patches.append([['cases',i,'wireHex'],result.hex()])
        if name in ('robinhood_prepare_edges','robinhood_submit_edges'):
            rows=fixture['cases']
            for i,row in enumerate(rows):
                if row['name'] not in ('alchemy-echo-raw-text','alchemy-echo-raw-text-protobuf','result-raw-text-json','result-raw-text-protobuf','echo-submit','prepare-echo'): continue
                if row['name']=='echo-submit':
                    for j,call in enumerate(row['upstream']):
                        if call.get('body'):
                            text=json_strings(call['body'])
                            body=parse(text)
                            prepared=next(r for r in rows if r['name']=='prepare-echo')
                            source=next(json.loads(v[0]['body'])['result'] for k,v in prepared['providers'].items() if 'wallet_prepareCalls' in k)
                            body['params'][0]['details']=order_like(body['params'][0]['details'],source['details'])
                            patches.append([['cases',i,'upstream',j,'body'],dump(body)])
                    continue
                # Locate the same JSON outcome for the wire twin; ticket/expiry
                # values differ, so use decoded wire twin values when supplied.
                base=row if row.get('responseText') else next(r for r in rows if r['name']==row['name'].replace('-protobuf','').replace('result-raw-text','result-raw-text-json'))
                result=parse(json_strings(base['responseText']))
                source=next(json.loads(v[0]['body'])['result'] for k,v in row['providers'].items() if ('wallet_prepareCalls' in k or 'wallet_sendPreparedCalls' in k))
                if 'prepared' in result:
                    result['prepared']['details']=order_like(result['prepared']['details'],source['details'])
                    if row is not base:
                        decoded=row['decoded']['payload']
                        result['ticket']=decoded['ticket'];result['expiresAt']=decoded['expiresAt']
                else: result=order_like(result,source)
                text=dump(result)
                if row.get('responseText'):
                    patches.append([['cases',i,'responseText'],text])
                    if row.get('protoOfJson'): patches.append([['cases',i,'protoOfJson'],encode_structured(json.loads(text)).hex()])
                else:
                    wire_value=json.loads(text)
                    if name=='robinhood_prepare_edges': wire_value['prepared']['details']['n3']=-0.0  # source -0, retained on protobuf wire
                    patches.append([['cases',i,'wireHex'],encode_structured(wire_value).hex()])
        if name=='news_reads_edges':
            for i,row in enumerate(fixture['units']['pricesValue']):
                if not row.get('value'): continue
                try: source=json.loads(row['body']).get('prices',[])
                except (ValueError,AttributeError): continue
                text=dump(order_like(parse(json_strings(row['value'])),source))
                if text!=row['value']: patches.append([['units','pricesValue',i,'value'],text])
        if name=='robinhood_diagnose_edges':
            for path,text in patches[:]:
                if path[-1]=='responseText':
                    i=path[1]
                    if fixture['cases'][i].get('protoOfJson'): patches.append([['cases',i,'protoOfJson'],encode_structured(json.loads(text)).hex()])
        overlays[name+'.json'] = patches
    (ROOT/'tests/fixtures/strict_contract_cases.json').write_text(json.dumps(overlays,ensure_ascii=False,indent=2)+'\n')
    print({k:len(v) for k,v in overlays.items()})
if __name__ == '__main__': main()
