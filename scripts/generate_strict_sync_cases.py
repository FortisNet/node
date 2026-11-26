#!/usr/bin/env python3
"""Pin the sync key insertion-order change without re-encoding numbers or
any unrelated protobuf field. Frozen fixture request bytes stay unchanged."""
import json
from generate_strict_query_cases import ROOT


def varint(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def records(data):
    def read(at):
        value, shift = 0, 0
        while True:
            b = data[at]; at += 1
            value |= (b & 127) << shift
            if b < 128: return value, at
            shift += 7
    at = 0
    while at < len(data):
        start = at
        tag, at = read(at)
        wire = tag & 7
        if wire == 2:
            size, at = read(at); payload = data[at:at+size]; at += size
        elif wire == 0:
            _, at = read(at); payload = None
        elif wire in (1, 5):
            at += 8 if wire == 1 else 4; payload = None
        else: raise ValueError(wire)
        assert at <= len(data)
        yield tag >> 3, wire, payload, data[start:at]


def replace_field(data, field, transform):
    out = []
    for n, wire, payload, raw in records(data):
        if n == field and wire == 2:
            changed = transform(payload)
            if changed != payload: raw = varint(n * 8 + 2) + varint(len(changed)) + changed
        out.append(raw)
    return b''.join(out)


def order_key(data):
    entries = list(records(data))
    names = [next(p.decode() for n,w,p,_ in records(p) if n==1 and w==2) for _,_,p,_ in entries]
    if not {'network', 'address', '7', '10'}.issubset(names): return data
    order = ['network', 'address'] + [n for n in names if n not in ('network', 'address')]
    return b''.join(entries[names.index(n)][3] for n in order)


def main():
    fixture = json.loads((ROOT/'tests/corpus/sync_edges.json').read_text())
    changes = {}
    for case in fixture['cases']:
        if case['name'] in ('mixedJson', 'mixedModelsExplicit'):
            body = case['bodyText']; document = json.loads(body)
            key = document['changes'][6]['key']
            ordered = {k: key[k] for k in ('network', 'address')}
            ordered.update({k:v for k,v in key.items() if k not in ordered})
            text = lambda d: json.dumps(d, ensure_ascii=False, separators=(',', ':'))
            assert body.count(text(key)) == 1
            changed = body.replace(text(key), text(ordered))
            assert len(changed.encode()) == len(body.encode())
            changes[case['name']] = {'bodyText': changed}
        elif case['name'] == 'mixedProtobuf':
            original = bytes.fromhex(case['wireHex'])
            changed = replace_field(original, 7, lambda c: replace_field(c, 2, order_key))
            assert changed != original and len(changed) == len(original)
            changes[case['name']] = {'wireHex': changed.hex()}
    assert len(changes) == 3
    (ROOT/'tests/fixtures/strict_sync_order_cases.json').write_text(json.dumps(changes, indent=2)+'\n')


if __name__ == '__main__': main()
