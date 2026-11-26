#!/usr/bin/env python3
"""Independent expected request deltas. Never reads native test output."""
import json
import re
from pathlib import Path
from generate_strict_query_cases import first_values, ROOT


def digits(value):
    return re.fullmatch('[0-9]+', value) is not None


def main():
    changes = {}
    def fixture(name):
        return json.loads((ROOT / f'tests/corpus/{name}.json').read_text())
    address = {}
    for row in fixture('address_transfers_edges')['units']['query']:
        query = row['query']
        try:
            values, repeated = first_values('/?' + query)
            reject = any(k in values and not digits(values[k]) for k in ('limit', 'from', 'to'))
        except (UnicodeError, ValueError):
            reject, values, repeated = True, {}, set()
        if reject:
            address[query] = {'valid': False}
        elif repeated & {'since', 'before', 'refresh', 'limit', 'token', 'priceId', 'from', 'to', 'flow'}:
            value = {'limit': int(values.get('limit', '100'))}
            for k in ('since', 'before', 'token', 'flow'):
                if k in values: value[k] = values[k]
            for k in ('from', 'to'):
                if k in values: value[k] = int(values[k])
            if 'priceId' in values: value['priceId'] = values['priceId'].strip().lower()
            if 'refresh' in values: value['refresh'] = values['refresh'] == 'true'
            # Duplicate cases in this frozen corpus all have valid first
            # values; leave all unrelated schema decisions to the old rows.
            address[query] = {'valid': True, 'value': value}
    changes['address'] = address
    news = {}
    known = {'trend', 'macro', 'policy', 'chains', 'protocol', 'security', 'exchanges', 'defi', 'nft'}
    for row in fixture('news_reads_edges')['units']['feedQuery']:
        query = row['query']
        try:
            values, repeated = first_values('/' + query)
            reject = 'limit' in values and (not digits(values['limit']) or not 1 <= int(values['limit']) <= 100)
        except (UnicodeError, ValueError):
            reject, values, repeated = True, {}, set()
        if reject:
            news[query] = {'status': 400, 'body': '{"error":"BAD_REQUEST"}'}
        elif repeated & {'topics', 'limit'}:
            topics = sorted(x.strip().lower() for x in values.get('topics', '').split(',') if x.strip().lower() in known)
            news[query] = {'status': 200, 'key': 'news:' + '+'.join(topics) + '|' + values.get('limit', '40')}
    changes['news'] = news
    changes['search'] = {name: {'term': value} for name, value in {
        'q-repeated': 'a', 'q-repeated-same': 'usd', 'q-repeated-empty': 'usd',
        'term-bad-escape': None, 'term-invalid-utf8': None, 'term-lone-surrogate': None}.items()}
    changes['page'] = {}
    for case in fixture('token_page_edges')['cases']:
        values = case.get('query', {})
        if not isinstance(values, dict): continue
        first = {k: v[0] if isinstance(v, list) else v for k, v in values.items()}
        if 'days' in first and not digits(first['days']):
            changes['page'][case['name']] = {'strictExpected': 'invalid'}
        elif any(isinstance(v, list) for v in values.values()):
            changes['page'][case['name']] = {'strictExpected': 'ok'}
    (ROOT / 'tests/fixtures/strict_unit_expectations.json').write_text(json.dumps(changes, indent=2)+'\n')
    print({k: len(v) for k,v in changes.items()})


if __name__ == '__main__':
    main()
