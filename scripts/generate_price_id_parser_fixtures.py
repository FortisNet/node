#!/usr/bin/env python3
"""Generate the price-id parser test fixtures from contracts/price-id-fixtures.json.

The contract file holds reference cases for parsing price ids (for example
"ethereum" or a network-and-token-address form): the raw text, what the
earlier TypeScript service made of it, what the native parser is expected to
make of it, and a note where the two deliberately differ. This tool writes
those cases as a C++ table in tests/price_id_parser_fixtures.h, which
tests/price_id_parser_test.cpp includes.

  python3 scripts/generate_price_id_parser_fixtures.py           rewrite the header
  python3 scripts/generate_price_id_parser_fixtures.py --write   the same
  python3 scripts/generate_price_id_parser_fixtures.py --check   fail if it is stale
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = ROOT / 'contracts/price-id-fixtures.json'
OUTPUT_PATH = ROOT / 'tests/price_id_parser_fixtures.h'

TEXT_FIELDS = ('name', 'raw', 'tsKind', 'tsSource', 'tsNetwork', 'tsAddress', 'tsStandard',
               'nativeKind', 'gap')
_SURROGATE = re.compile('[\ud800-\udfff]')

HEADER = '''// Generated from contracts/price-id-fixtures.json by
// scripts/generate_price_id_parser_fixtures.py. Historical reference cases.
#pragma once

#include "../src/providers/price_id_parser.h"

struct PriceIdParserFixture {
  const char* name;
  const char* raw;
  fortis::providers::PriceIdKind typescript_kind;
  const char* typescript_source_id;
  const char* typescript_network;
  const char* typescript_address;
  const char* typescript_standard;
  fortis::providers::PriceIdKind native_kind;
  const char* documented_gap;
};

constexpr PriceIdParserFixture kPriceIdParserFixtures[] = {
'''


def parse_json(source):
    """Parse strict JSON; Python would otherwise also accept NaN and Infinity."""
    def reject(name):
        raise ValueError(f'{name} is not valid JSON')
    return json.loads(source, parse_constant=reject)


def read_rows(source):
    rows = parse_json(source)
    if not isinstance(rows, list):
        raise ValueError('price-id fixtures must be a JSON array')
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError(f'price-id fixture {index} must be an object')
        for name in TEXT_FIELDS:
            if not isinstance(row.get(name), str):
                raise ValueError(f'price-id fixture {index} needs a string "{name}"')
    return rows


def c_string(value):
    """Encode UTF-8 bytes with fixed-width octal escapes.

    C++ hex escapes can absorb following hex digits; octal escapes are always
    limited to three digits. An unpaired surrogate becomes U+FFFD.
    """
    result = '"'
    for byte in _SURROGATE.sub('�', value).encode('utf-8'):
        if byte == 0x22:
            result += '\\"'
        elif byte == 0x5C:
            result += '\\\\'
        elif 0x20 <= byte <= 0x7E:
            result += chr(byte)
        else:
            result += f'\\{byte:03o}'
    return result + '"'


def entry(item):
    kind = 'fortis::providers::PriceIdKind::'
    return (f"  {{{c_string(item['name'])}, {c_string(item['raw'])}, {kind}{item['tsKind']}, "
            f"{c_string(item['tsSource'])}, {c_string(item['tsNetwork'])}, "
            f"{c_string(item['tsAddress'])}, {c_string(item['tsStandard'])}, "
            f"{kind}{item['nativeKind']}, {c_string(item['gap'])}}},")


def render(rows):
    return HEADER + '\n'.join(entry(row) for row in rows) + '\n};\n'


def read_text(path):
    return path.read_bytes().decode('utf-8', 'replace')


def run(arguments):
    mode = arguments[0] if arguments else '--write'
    if mode not in ('--write', '--check'):
        raise ValueError(
            'usage: python3 scripts/generate_price_id_parser_fixtures.py [--write|--check]')
    rows = read_rows(read_text(SOURCE_PATH))
    generated = render(rows)
    if mode == '--check':
        if read_text(OUTPUT_PATH) != generated:
            sys.stderr.write('price-id parser fixtures are stale; run '
                             'python3 scripts/generate_price_id_parser_fixtures.py\n')
            return 1
        print(f'price-id parser fixtures match native-owned reference data ({len(rows)} fixtures)')
        return 0
    OUTPUT_PATH.write_bytes(generated.encode('utf-8'))
    print(f'Wrote {len(rows)} fixtures from native-owned reference data to {OUTPUT_PATH}')
    return 0


def main(arguments):
    try:
        return run(arguments)
    except Exception as error:  # every failure is a one-line message and status 1
        sys.stderr.write(f'{error}\n')
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
