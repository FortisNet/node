"""Exact frozen-response edits for ASCII-only market identifiers (js-case)."""
import json


def array_rows(text, start=0):
    """Decoded rows with their original byte-equivalent JSON text."""
    assert text[start] == '['
    at, decoder = start + 1, json.JSONDecoder()
    while text[at] != ']':
        row, end = decoder.raw_decode(text, at)
        yield row, text[at:end]
        at = end + (text[end] == ',')


def json_without_markets(text, removed):
    start = len('{"markets":')
    rows = list(array_rows(text, start))
    # The fixture's compact JSON has no whitespace between array entries.
    end = start + 1 + sum(len(raw) for _, raw in rows) + max(0, len(rows) - 1)
    return text[:start + 1] + ','.join(raw for row, raw in rows if row['id'] not in removed) + text[end:]


def proto_without_markets(wire, rows, removed):
    """Keep unaffected Struct wire bytes; a fresh row now joins cached ones.

    The pinned fixtures use only length-delimited outer fields and a cache
    message starting with hit=true. Refuse a changed fixture encoding.
    """
    at, chunks, market_index = 0, [], 0
    while at < len(wire):
        start, tag = at, wire[at]
        at += 1
        size, shift = 0, 0
        while True:
            byte = wire[at]
            at += 1
            size |= (byte & 127) << shift
            shift += 7
            if not byte & 128:
                break
        data = wire[at:at + size]
        at += size
        if tag == 10:
            row = rows[market_index]
            market_index += 1
            if row['id'] not in removed:
                chunks.append(wire[start:at])
        else:
            assert tag == 26 and data.startswith(b'\x08\x01'), 'unexpected fixture cache encoding'
            data = data[2:] + b'\x18\x01'  # hit=false (omitted), partial=true
            assert len(data) == size
            chunks.append(wire[start:at - size] + data)
    assert market_index == len(rows)
    return b''.join(chunks)
