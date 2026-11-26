#!/usr/bin/env python3
"""Generate the built-in network table from contracts/network-catalog.json.

The catalog lists every network the node knows at start-up (its id, family,
public RPC endpoints, native coin and so on) and the ten capability names each
network reports. This tool checks the catalog against the fixed capacities of
the native code and writes it as C++ data in src/networks/network_registry.cpp.

  python3 scripts/generate_network_catalog.py --write   rewrite the C++ file
  python3 scripts/generate_network_catalog.py --check   fail if it is stale

The generated file is committed, so --check is what the build runs.

The capacity check on the network-list response has to predict the exact
number of bytes the native JSON writer will produce. It does that by
serializing the same document here with the ECMAScript JSON rules (see
json_stringify), which are the rules the limits were originally set against;
Python's own json.dumps formats some numbers and characters differently.
"""
import json
import math
import os
import re
import sys
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = ROOT / 'contracts/network-catalog.json'
OUTPUT_PATH = ROOT / 'src/networks/network_registry.cpp'

# Capacities of the fixed-size native structures (src/networks/).
LIMITS = {
    'networks': 43,
    'idBytes': 64,
    'familyBytes': 23,
    'rpcUrls': 5,
    'graphqlUrls': 3,
    'urlBytes': 255,
    'nativeSymbolBytes': 8,
    'coingeckoIdBytes': 64,
    'capabilityKeyBytes': 15,
    'capabilityValueBytes': 4,
}
# Keep this in sync with kJsonCapacity in src/operations/network_route.cpp. The native
# writer needs one extra byte for its terminating NUL.
NATIVE_JSON_CAPACITY = 32768
UINT32_MAX = 0xFFFFFFFF


class Missing:
    """The value of a catalog field that is not present at all.

    JSON null is a different thing: a field that is present and null is an
    error for most fields, while a missing optional field is fine.
    """

    def __bool__(self):
        return False

    def __repr__(self):
        return 'undefined'


MISSING = Missing()

_SURROGATE = re.compile('[\ud800-\udfff]')
_ARRAY_INDEX = re.compile('0|[1-9][0-9]{0,9}')
_SHORT_ESCAPES = {'"': '\\"', '\\': '\\\\', '\b': '\\b', '\t': '\\t', '\n': '\\n',
                  '\f': '\\f', '\r': '\\r'}


def parse_json(source):
    """Parse strict JSON; Python would otherwise also accept NaN and Infinity."""
    def reject(name):
        raise ValueError(f'{name} is not valid JSON')
    return json.loads(source, parse_constant=reject)


def field(value, name):
    """Read one named field of a parsed JSON value, or MISSING."""
    if value is None or value is MISSING:
        kind = 'null' if value is None else 'undefined'
        raise TypeError(f"Cannot read properties of {kind} (reading '{name}')")
    if isinstance(value, dict):
        return value.get(name, MISSING)
    return MISSING


def absent(value):
    """True for a field that is missing or null."""
    return value is None or value is MISSING


def or_empty(value):
    return [] if absent(value) else value


def is_integer(value):
    """True for a JSON number with no fractional part (1 and 1.0 both count)."""
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return True
    return isinstance(value, float) and value.is_integer()


def is_uint32(value):
    return is_integer(value) and 0 <= value <= UINT32_MAX


def utf8(value):
    """UTF-8 bytes of a string; an unpaired surrogate becomes U+FFFD."""
    return _SURROGATE.sub('�', value).encode('utf-8')


def number_to_string(value):
    """Format a number as ECMAScript does (Number::toString, radix 10)."""
    try:
        value = float(value)
    except OverflowError:
        return 'null'
    if math.isnan(value) or math.isinf(value):
        return 'null'  # what JSON.stringify writes for a non-finite number
    if value == 0:
        return '0'
    sign = '-' if value < 0 else ''
    # repr() gives the shortest digits that round-trip, as ECMAScript requires.
    _, digit_tuple, exponent = Decimal(repr(abs(value))).as_tuple()
    digits = ''.join(map(str, digit_tuple)).lstrip('0')
    stripped = digits.rstrip('0')
    exponent += len(digits) - len(stripped)
    digits = stripped
    count = len(digits)
    point = count + exponent  # position of the decimal point within digits
    if count <= point <= 21:
        return sign + digits + '0' * (point - count)
    if 0 < point <= 21:
        return sign + digits[:point] + '.' + digits[point:]
    if -6 < point <= 0:
        return sign + '0.' + '0' * -point + digits
    power = point - 1
    mantissa = digits[0] + ('.' + digits[1:] if count > 1 else '')
    return f"{sign}{mantissa}e{'+' if power > 0 else '-'}{abs(power)}"


def quote_json_string(text):
    out = ['"']
    for char in text:
        code = ord(char)
        if char in _SHORT_ESCAPES:
            out.append(_SHORT_ESCAPES[char])
        elif code < 0x20 or 0xD800 <= code <= 0xDFFF:
            out.append(f'\\u{code:04x}')
        else:
            out.append(char)
    out.append('"')
    return ''.join(out)


def _is_array_index(key):
    return _ARRAY_INDEX.fullmatch(key) is not None and int(key) < UINT32_MAX


def json_stringify(value):
    """Serialize parsed JSON exactly as ECMAScript JSON.stringify does.

    No spaces; whole numbers without a fraction; non-ASCII text left as is;
    object keys that look like array indexes first, in numeric order, then the
    rest in the order they were written.
    """
    if value is None:
        return 'null'
    if value is True:
        return 'true'
    if value is False:
        return 'false'
    if isinstance(value, str):
        return quote_json_string(value)
    if isinstance(value, (int, float)):
        return number_to_string(value)
    if isinstance(value, list):
        return '[' + ','.join(json_stringify(item) for item in value) + ']'
    indexes = sorted((key for key in value if _is_array_index(key)), key=int)
    names = [key for key in value if not _is_array_index(key)]
    return '{' + ','.join(quote_json_string(key) + ':' + json_stringify(value[key])
                          for key in indexes + names) + '}'


def read_catalog(source):
    catalog = parse_json(source)
    validate_catalog(field(catalog, 'seeds'), field(catalog, 'capabilities'))
    return catalog


def assert_string(value, label, maximum, optional=False):
    if value is MISSING and optional:
        return
    if not isinstance(value, str) or len(value) == 0 or len(utf8(value)) > maximum:
        raise ValueError(f'{label} must be a non-empty UTF-8 string of at most {maximum} bytes')


def native_json_length(seeds, capabilities):
    """Byte length of the network-list response the native code will write."""
    networks = []
    for seed in seeds:
        network = {'id': field(seed, 'id'), 'family': field(seed, 'family')}
        chain_id = field(seed, 'evmChainId')
        if chain_id is not MISSING:
            network['evmChainId'] = chain_id
        network['testnet'] = field(seed, 'testnet') is True
        confirmations = field(seed, 'requiredConfirmations')
        if confirmations is not MISSING:
            network['requiredConfirmations'] = confirmations
        network['rpcUrls'] = or_empty(field(seed, 'rpcUrls'))
        graphql_urls = field(seed, 'graphqlUrls')
        if not absent(graphql_urls) and len(graphql_urls):
            network['graphqlUrls'] = graphql_urls
        native = field(seed, 'native')
        if native:
            network['native'] = native
        network['capabilities'] = {name: 'none' for name in capabilities}
        networks.append(network)
    body = {
        'networks': networks,
        'prices': {'spot': False, 'history': False,
                   'sources': {'coingecko': 'keyless', 'jupiter': 'keyed', 'vestige': 'keyless'}},
    }
    # json_stringify uses two-byte short escapes for these five controls;
    # Writer::string uses six-byte \u00xx escapes. All other byte lengths match.
    short_escapes = 0

    def visit(value):
        nonlocal short_escapes
        if isinstance(value, str):
            short_escapes += sum(1 for char in value if char in '\b\t\n\f\r')
        elif isinstance(value, list):
            for item in value:
                visit(item)
        elif isinstance(value, dict):
            for key, item in value.items():
                visit(key)
                visit(item)

    visit(body)
    return len(utf8(json_stringify(body))) + 4 * short_escapes


def _distinct_count(values):
    """How many different values a list holds; containers count by identity."""
    seen = set()
    for value in values:
        if isinstance(value, bool) or value is None:
            seen.add(('literal', value))
        elif isinstance(value, str):
            seen.add(('string', value))
        elif isinstance(value, (int, float)):
            seen.add(('number', number_to_string(value)))
        else:
            seen.add(('object', id(value)))
    return len(seen)


def validate_catalog(seeds, capabilities):
    if not isinstance(seeds, list) or len(seeds) == 0 or len(seeds) > LIMITS['networks']:
        raise ValueError(f"network count must be between 1 and {LIMITS['networks']}")
    ids = set()
    for seed in seeds:
        seed_id = field(seed, 'id')
        assert_string(seed_id, 'network id', LIMITS['idBytes'])
        assert_string(field(seed, 'family'), f'{seed_id}.family', LIMITS['familyBytes'])
        if seed_id in ids:
            raise ValueError(f'duplicate network id {seed_id}')
        ids.add(seed_id)
        for name, maximum in (('rpcUrls', LIMITS['rpcUrls']),
                              ('graphqlUrls', LIMITS['graphqlUrls'])):
            urls = or_empty(field(seed, name))
            if not isinstance(urls, list) or len(urls) > maximum:
                raise ValueError(f'{seed_id}.{name} exceeds {maximum} entries')
            for index, url in enumerate(urls):
                assert_string(url, f'{seed_id}.{name}[{index}]', LIMITS['urlBytes'])
        chain_id = field(seed, 'evmChainId')
        if chain_id is not MISSING and not is_uint32(chain_id):
            raise ValueError(f'{seed_id}.evmChainId must fit uint32')
        confirmations = field(seed, 'requiredConfirmations')
        if confirmations is not MISSING and not is_uint32(confirmations):
            raise ValueError(f'{seed_id}.requiredConfirmations must fit uint32')
        native = field(seed, 'native')
        if native is not MISSING:
            assert_string(field(native, 'symbol'), f'{seed_id}.native.symbol',
                          LIMITS['nativeSymbolBytes'])
            if not is_uint32(field(native, 'decimals')):
                raise ValueError(f'{seed_id}.native.decimals must fit uint32')
            assert_string(field(native, 'coingeckoId'), f'{seed_id}.native.coingeckoId',
                          LIMITS['coingeckoIdBytes'], optional=True)
    if (not isinstance(capabilities, list) or len(capabilities) != 10 or
            _distinct_count(capabilities) != len(capabilities)):
        raise ValueError('NONE must define exactly ten unique capability names')
    for name in capabilities:
        assert_string(name, 'capability name', LIMITS['capabilityKeyBytes'])
    for status in ('none', 'ok', 'beta'):
        assert_string(status, 'capability value', LIMITS['capabilityValueBytes'])
    if native_json_length(seeds, capabilities) >= NATIVE_JSON_CAPACITY:
        raise ValueError(f'native network JSON exceeds {NATIVE_JSON_CAPACITY - 1} bytes')
    return True


def cpp_string(value):
    """Encode UTF-8 bytes with fixed-width octal escapes.

    C++ hex escapes can absorb following hex digits; octal escapes are always
    limited to three digits.
    """
    result = '"'
    for byte in utf8(value):
        if byte == 0x22:
            result += '\\"'
        elif byte == 0x5C:
            result += '\\\\'
        elif 0x20 <= byte <= 0x7E:
            result += chr(byte)
        else:
            result += f'\\{byte:03o}'
    return result + '"'


def pointer_array(name, values):
    if not values:
        return ''
    lines = '\n'.join(f'  {cpp_string(value)},' for value in values)
    return f'static const char* const {name}[] = {{\n{lines}\n}};\n'


def _uint(value):
    """A checked whole number (possibly missing) as a C++ unsigned literal."""
    return f'{0 if absent(value) else int(value)}U'


def render_catalog(seeds, capabilities):
    validate_catalog(seeds, capabilities)
    out = ('// Generated by scripts/generate_network_catalog.py from\n'
           '// contracts/network-catalog.json. Do not edit by hand.\n'
           '#include "network_registry.h"\n\nnamespace fortis::networks {\n\nnamespace {\n')
    for i, seed in enumerate(seeds):
        out += pointer_array(f'kRpc{i}', or_empty(field(seed, 'rpcUrls')))
        out += pointer_array(f'kGraphql{i}', or_empty(field(seed, 'graphqlUrls')))
    out += '\nstatic const NativeNetworkConfig kNetworks[] = {\n'
    for i, seed in enumerate(seeds):
        native = field(seed, 'native')
        chain_id = field(seed, 'evmChainId')
        confirmations = field(seed, 'requiredConfirmations')
        rpc_urls = or_empty(field(seed, 'rpcUrls'))
        graphql_urls = or_empty(field(seed, 'graphqlUrls'))
        coingecko_id = field(native, 'coingeckoId') if native else MISSING
        fields = [
            cpp_string(field(seed, 'id')), cpp_string(field(seed, 'family')),
            'false' if chain_id is MISSING else 'true', _uint(chain_id),
            'true' if field(seed, 'testnet') is True else 'false',
            'false' if confirmations is MISSING else 'true', _uint(confirmations),
            f'kRpc{i}' if rpc_urls else 'nullptr', f'{len(rpc_urls)}U',
            f'kGraphql{i}' if graphql_urls else 'nullptr', f'{len(graphql_urls)}U',
            cpp_string(field(native, 'symbol')) if native else 'nullptr',
            _uint(field(native, 'decimals') if native else MISSING),
            'nullptr' if coingecko_id is MISSING else cpp_string(coingecko_id),
            '{' + ', '.join('"none"' for _ in capabilities) + '}',
        ]
        out += f"  {{{', '.join(fields)}}},\n"
    out += '};\n}  // namespace\n\n'
    names = '\n'.join(f'  {cpp_string(name)},' for name in capabilities)
    out += f'const char* const kNetworkCapabilityNames[kNetworkCapabilityCount] = {{\n{names}\n}};\n\n'
    out += 'const NativeNetworkConfig* NativeNetworkRegistry::list(size_t* count) const {\n'
    out += '  if (count) *count = sizeof(kNetworks) / sizeof(kNetworks[0]);\n'
    out += '  return kNetworks;\n}\n\n}  // namespace fortis::networks\n'
    return out


def read_text(path):
    return path.read_bytes().decode('utf-8', 'replace')


def run(arguments):
    mode = arguments[0] if arguments else None
    if mode not in ('--write', '--check'):
        raise ValueError('usage: python3 scripts/generate_network_catalog.py --write|--check')
    catalog = read_catalog(read_text(SOURCE_PATH))
    seeds, capabilities = catalog['seeds'], catalog['capabilities']
    generated = render_catalog(seeds, capabilities)
    if mode == '--write':
        OUTPUT_PATH.write_bytes(generated.encode('utf-8'))
        relative = os.path.relpath(OUTPUT_PATH, ROOT)
        sys.stdout.write(f'generated {relative} ({len(seeds)} networks)\n')
        return 0
    if read_text(OUTPUT_PATH) != generated:
        sys.stderr.write('native network catalog is stale; run make generate-network-catalog\n')
        return 1
    sys.stdout.write(f'native network catalog matches native contract ({len(seeds)} networks)\n')
    return 0


def main(arguments):
    try:
        return run(arguments)
    except Exception as error:  # every failure is a one-line message and status 1
        sys.stderr.write(f'{error}\n')
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
