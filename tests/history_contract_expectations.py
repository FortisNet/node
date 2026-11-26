"""Current price decimal/first-query-value contracts over historical replay data.

Only documented query grammar changes are retargeted. Captured files, provider
payloads, cache assertions and wire oracles remain intact.
"""
import copy
import json
import re
import string
from urllib.parse import parse_qs, unquote_to_bytes, urlsplit

FIXTURE_PROVIDER = {'COINGECKO_API_KEY': 'fixture-coingecko-pro-key', 'COINGECKO_TIER': 'pro'}


def refusal(case, detail=None):
    response = case['response']
    payload = {'error': 'BAD_REQUEST'}
    if detail:
        payload['detail'] = detail
    body = json.dumps(payload, separators=(',', ':'))
    headers = {key: value for key, value in response['headers'].items()
               if key not in ('vary', 'etag', 'retry-after')}
    headers.update({'content-length': str(len(body)), 'cache-control': 'no-store'})
    case['response'] = {'status': 400, 'headers': headers, 'body': body}
    case['upstreamCalls'] = []


def cached_first_value(case, reference):
    remaining = case['response']['headers'].get('x-rate-limit-remaining')
    case['response'] = copy.deepcopy(reference['response'])
    if remaining is not None:
        case['response']['headers']['x-rate-limit-remaining'] = remaining
    case['upstreamCalls'] = []


def history(fixture):
    group = next(group for group in fixture['groups'] if group['name'] == 'query')
    reference = next(case for case in group['cases'] if case['name'] == 'cached hourly')
    for case in group['cases']:
        query = urlsplit(case['urls'][0]).query
        params = parse_qs(query, keep_blank_values=True)
        if case['name'] in ('repeated id', 'repeated from', 'repeated to and interval'):
            cached_first_value(case, reference)
        elif re.search(r'%(?![0-9a-fA-F]{2})', query):
            refusal(case)
        elif case['name'] == 'id non-ASCII network lower-cased':
            payload = json.loads(case['response']['body'])
            payload['id'] = params['id'][0].translate(str.maketrans(string.ascii_uppercase, string.ascii_lowercase))
            case['response']['body'] = json.dumps(payload, separators=(',', ':'), ensure_ascii=False)
            case['response']['headers']['content-length'] = str(len(case['response']['body'].encode()))
        elif case['name'] == 'id Kelvin sign lowers to k':
            refusal(case, 'unrecognized id')
        elif case['response']['status'] == 200 and any(
                key in params and not re.fullmatch(r'[0-9]+', params[key][0]) for key in ('from', 'to')):
            refusal(case)
    return fixture


def ohlc(fixture):
    groups = {group['name']: group for group in fixture['groups']}
    reference = next(case for case in groups['days']['cases'] if case['name'] == 'days 7')
    for group in fixture['groups']:
        for case in group['cases']:
            if len(case['urls']) != 1 or 'response' not in case:
                continue
            query = urlsplit(case['urls'][0]).query
            params = parse_qs(query, keep_blank_values=True)
            if (group['name'], case['name']) in {
                    ('bad-days', 'days repeated'), ('bad-days', 'fiat repeated'), ('id', 'id repeated')}:
                cached_first_value(case, reference)
            elif group['name'] == 'id' and case['name'] == 'id malformed escape kept raw':
                refusal(case)
            elif group['name'] == 'id' and case['name'] == 'id Kelvin sign lowers to k':
                refusal(case, 'unsupported price id')
            elif case['response']['status'] == 200 and 'days' in params and not re.fullmatch(r'[0-9]+', params['days'][0]):
                refusal(case)
    return fixture


def reorder_map_response(response, ids, text_key='body', wire_key='bodyHex',
                         member='series', field=4):
    """Preserve captured values while replacing JS array-index key ordering."""
    from public_price_device_probe import fields, pb_bytes, text_field, varint
    if text_key in response:
        payload = json.loads(response[text_key])
        if not isinstance(payload, dict) or member not in payload:
            return
        original = payload[member]
        assert set(original) <= set(ids)
        payload[member] = {key: original[key] for key in ids if key in original}
        response[text_key] = json.dumps(payload, separators=(',', ':'), ensure_ascii=False)
        return
    parsed = fields(bytes.fromhex(response[wire_key]))
    entries = {text_field(value, 1): value for number, wire, value in parsed if number == field and wire == 2}
    assert set(entries) <= set(ids)
    encoded = []
    emitted = False
    for number, wire, value in parsed:
        if number == field and wire == 2:
            if not emitted:
                encoded.extend(pb_bytes(field, entries[key]) for key in ids if key in entries)
                emitted = True
        elif wire == 2:
            encoded.append(pb_bytes(number, value))
        else:
            encoded.append(varint(number << 3 | wire) + (varint(value) if wire == 0 else value))
    response[wire_key] = b''.join(encoded).hex()


def batch_edges(fixture):
    for group in fixture['groups']:
        for case in group['cases']:
            if group['name'] == 'fields' and case['name'] in {
                    'from single element array', 'from nested array', 'from hexadecimal string'}:
                refusal(case)
            if group['name'] == 'order' and case['name'] in {
                    'array index keys first', 'array index keys protobuf'}:
                ids = list(dict.fromkeys(json.loads(bytes.fromhex(case['requests'][0]['hex']))['ids']))
                reorder_map_response(case['response'], ids)
    return fixture


def batch(fixture):
    from public_price_device_probe import fields
    for name, case in fixture['cases'].items():
        if name in ('stringNumbers', 'exponentString', 'booleanFrom'):
            # request-coercion: no JSON string-to-number coercion, including whitespace/exponents.
            body = '{"error":"BAD_REQUEST"}'
            case['status'] = 400
            case['body'] = {'text': body}
            case['headers'] = {key: value for key, value in case['headers'].items() if key != 'vary'}
            case['headers']['content-length'] = str(len(body))
            case['providerCalls'] = []
            continue
        if case['status'] != 200:
            continue
        request = case['request']
        if request['contentType'] == 'application/json' and 'text' in request:
            requested = json.loads(request['text'])['ids']
        elif request['contentType'] == 'application/protobuf' and 'hex' in request:
            requested = [value.decode('utf-8') for number, wire, value in fields(bytes.fromhex(request['hex']))
                         if number == 1 and wire == 2]
        else:
            continue
        ids = list(dict.fromkeys(requested))
        if any(re.fullmatch(r'[0-9]+', identifier) for identifier in ids):
            reorder_map_response(case['body'], ids, 'text', 'wireHex')
    return fixture


def spot(fixture):
    """Retarget strict UTF-8/first-value queries and insertion-ordered price maps."""
    group = next(group for group in fixture['groups'] if group['name'] == 'query')
    reference = next(case for case in group['cases'] if case['name'] == 'duplicate id')
    for case in group['cases']:
        query = urlsplit(case['url']).query
        params = parse_qs(query, keep_blank_values=True)
        if case['name'] in ('repeated ids', 'repeated fiat', 'fiat flag twice'):
            cached_first_value(case, reference)
            continue
        try:
            unquote_to_bytes(query).decode('utf-8')
            malformed = bool(re.search(r'%(?![0-9a-fA-F]{2})', query))
        except UnicodeDecodeError:
            malformed = True
        if malformed:
            refusal(case)
        elif case['name'] == 'Kelvin sign lower-cases to k':
            body = json.dumps({'fiat': 'usd', 'prices': {}, 'missing': [],
                               'invalid': ['Kaspa']}, separators=(',', ':'), ensure_ascii=False)
            case['response']['body'] = body
            case['response']['headers']['content-length'] = str(len(body.encode()))
            case['upstreamCalls'] = []
        elif case['name'] in ('integer-like key first', 'integer-like key protobuf',
                              'protobuf with q=0 is JSON'):
            ids = list(dict.fromkeys(params['ids'][0].split(',')))
            reorder_map_response(case['response'], ids, member='prices', field=2)
    return fixture
