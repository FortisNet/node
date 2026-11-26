#!/usr/bin/env python3
"""Explicit text/order expectation deltas, derived from frozen inputs with
Python's Unicode and JSON codecs, never from native test output."""
import json
import re
from urllib.parse import quote
from generate_strict_query_cases import ROOT


def well(value):
    return value.encode('utf-16-le', 'surrogatepass').decode('utf-16-le', 'replace')


def drop_lone(value):
    return ''.join(c for c in value if not 0xd800 <= ord(c) <= 0xdfff)


def prefix(value, maximum):
    result, units = '', 0
    for char in well(value):
        width = 2 if ord(char) > 0xffff else 1
        if units + width > maximum: break
        result += char
        units += width
    return result


def json_strings(text):
    def token(match):
        value = json.loads(match[0])
        normalized = well(value)
        return json.dumps(normalized, ensure_ascii=False) if normalized != value else match[0]
    return re.sub(r'"(?:[^"\\]|\\.)*"', token, text)


def main():
    def load(name): return json.loads((ROOT / f'tests/corpus/{name}.json').read_text())
    changes = {}
    f = load('cross_chain_corpus')
    snapshots = {}
    for row in f['snapshots']:
        if row['name'] not in ('long', 'longkey', 'order'): continue
        source = json.loads(row['text'])
        bounded = {}
        for key, value in source.items():
            if key == "__proto__": continue  # existing provider snapshot filter
            key = prefix(key, 128)
            if isinstance(value, str):
                units = len(well(value).encode('utf-16-le')) // 2
                value = prefix(value, 8192) + ('…[truncated]' if units > 8192 else '')
            bounded[key] = value
        snapshots[row['name']] = {'snapshot': json.dumps(bounded, ensure_ascii=False, separators=(',', ':'))}
    changes['cross_snapshots'] = snapshots
    f = load('ai_conversation_corpus')
    row = f['snapshots'][39]
    assert row['name'] == 'loneSurrogate'
    changes['ai_snapshots'] = {'39': {k: json_strings(row[k]) for k in ('normalized', 'context')}}
    changes['ai_replies'] = {'4': {'result': drop_lone(f['replies'][4]['result'])}}
    row = f['requests'][3]
    body = json.loads(row['body'])
    for turn in body['input']:
        if isinstance(turn.get('content'), str): turn['content'] = drop_lone(turn['content'])
    changes['ai_requests'] = {'3': {'body': json.dumps(body, ensure_ascii=False, separators=(',', ':'))}}
    f = load('robinhood_corpus')
    changes['robinhood_reverts'] = {'2': {'reason': drop_lone(f['reverts'][2]['reason'])}}
    f = load('tron_token_holdings_edges')
    changes['tron_projections'] = {str(i): {'output': json_strings(row['output'])}
        for i, row in enumerate(f['units']['projections']) if row.get('output') and json_strings(row['output']) != row['output']}
    f = load('address_nfts_edges')
    account = f['units']['account']
    changes['nft_keys'] = {}
    for i, row in enumerate(f['units']['keys']):
        if row.get('error') == 'URIError' and well(row['nftId']) != row['nftId']:
            key = 'nft/item/' + quote(account, safe="~!*'()-._") + '/' + quote(well(row['nftId']), safe="~!*'()-._")
            changes['nft_keys'][str(i)] = {'key': key, 'error': None}
    (ROOT/'tests/fixtures/strict_text_expectations.json').write_text(json.dumps(changes, indent=2, ensure_ascii=False)+'\n')
    # Only the three sanitizer truncation deltas need compiled overrides in
    # the bounded-document unit (it deliberately does not link the DOM).
    rows = []
    for row in load('token_ref_corpus')['sanitize']:
        for kind in ('symbol', 'name'):
            expected = row[kind]
            value = json.loads(expected['json'])
            normalized = drop_lone(value)
            if normalized != value:
                rows.append((row['inputHex'], 'sanitize'+kind.title(), json.dumps(normalized, ensure_ascii=False), normalized.encode().hex()))
    out = ['#pragma once', '#include <string.h>', '// Generated independent Unicode-boundary expectation deltas.', 'namespace strict {',
           'inline bool SanitizeCase(const char* input, const char* kind, const char** json, const char** hex) {',
           '  struct Row { const char* input; const char* kind; const char* json; const char* hex; };',
           '  static const Row rows[] = {']
    for row in rows: out.append('    {'+', '.join(json.dumps(v, ensure_ascii=False) for v in row)+'},')
    out += ['  };', '  for (const auto& row : rows) if (!strcmp(input, row.input) && !strcmp(kind, row.kind)) {',
            '    *json = row.json; *hex = row.hex; return true;', '  }', '  return false;', '}', '}', '']
    (ROOT/'tests/strict_sanitize_cases.h').write_text('\n'.join(out))
    print({k: len(v) for k,v in changes.items()}, 'sanitizers', len(rows))


if __name__ == '__main__': main()
