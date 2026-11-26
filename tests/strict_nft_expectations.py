"""Exact NFT expectations for Unicode-safe names and newly valid item keys."""
import copy
from datetime import datetime, timezone
import hashlib
import json
from urllib.parse import quote
from strict_contract_expectations import encode_structured


def apply(fixture):
    by_name={r['name']:r for r in fixture['cases']}
    def wire_text(row,document):
        row['responseText']=json.dumps(document,ensure_ascii=False,separators=(',',':'))
        row['headers']['content-length']=str(len(row['responseText'].encode()))
    names=json.loads(by_name['names']['responseText'])
    names['items'][2]['name']=names['items'][2]['name'].replace('�…','…')
    wire_text(by_name['names'],names)
    encoded=encode_structured(names)
    by_name['names-protobuf']['wireHex']=encoded.hex()
    by_name['names-protobuf']['headers']['content-length']=str(len(encoded))
    row=by_name['surrogate-token-id'];at=row['at']
    state=by_name['surrogate-token-state']['nft'][0]
    account=state['address'];contract=state['items'][0]['value']['contractAddress']
    # The provider's two token IDs are strings. The escaped lone surrogate
    # becomes one scalar replacement; both records now have valid URI keys.
    tokens=[item['tokenId'] for item in json.loads(row['upstream'][0]['body'])['ownedNfts']]
    tokens=[token.encode('utf-16-le','surrogatepass').decode('utf-16-le','replace') for token in tokens]
    records=[]
    for token in tokens:
        nft_id='eth-mainnet:'+contract+':'+token
        value={'accountId':'<account>','nftId':nft_id,'contractAddress':contract,'tokenId':token,
               'standard':None,'name':None,'collectionName':None,'sourceUrls':{'image':None},
               'flags':{},'firstSeenAt':at,'goneAt':None}
        records.append({'key':quote(nft_id,safe="~!*'()-._"),'expiresAt':at+7*86400000,'value':value})
    old=copy.deepcopy(state['items'][0]);old['value']['goneAt']=at;old['expiresAt']=at+7*86400000
    # Prior records were written during this replay, under the new scalar policy.
    for key in ('name',):
        if old['value'].get(key): old['value'][key]=old['value'][key].replace('���','�')
    old['value']['sourceUrls']['image']=old['value']['sourceUrls']['image'].replace('���','�')
    state['items']=sorted(records+[old],key=lambda record:record['key'].encode())
    state['scan']['value']={'lastScanAt':at,'lastFullScanAt':at,'lastError':None,'lastErrorAt':None}
    state['scan']['expiresAt']=at+7*86400000
    def iso(value): return datetime.fromtimestamp(value/1000,timezone.utc).isoformat(timespec='milliseconds').replace('+00:00','Z')
    items=[]
    for record in state['items']:
        value=record['value']
        if value['goneAt'] is not None: continue
        items.append({'id':value['nftId'],'contractAddress':contract,'tokenId':value['tokenId'],
                      'standard':None,'name':None,'collectionName':None,'media':{'image':None,'imageProxy':None},
                      'flags':{},'firstSeenAt':iso(at)})
    wire_text(row,{'network':'eth-mainnet','address':account,'items':items,'stale':False,'cache':{'hit':True,'timestamp':iso(at)}})
    return fixture
