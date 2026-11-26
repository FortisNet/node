#!/usr/bin/env python3
"""Generate the lookup tables used to redact secrets from diagnostics.

The redaction code (src/security/diagnostic_redaction.cpp) hides wallet recovery
phrases before text leaves the node. To see through disguises it strips
invisible characters, maps look-alike letters to plain ones and applies
Unicode normalization. This tool writes the data it needs as C++ tables in
src/security/redaction_tables.cpp, from three files under contracts/:

  redaction-data.json          invisible code points, look-alike letters and
                               the replacement text
  bip39-english.json           the 2,048 recovery-phrase words
  redaction-unicode-data.json  Unicode normalization data: compatibility
                               decompositions (NFKD), canonical composition
                               pairs and combining-mark ranks

The Unicode data is a frozen file on purpose. It records which Unicode version
it holds, so the output is the same on every machine and does not depend on
the Unicode tables of whichever language runtime happens to be installed. Do
not replace it with Python's unicodedata module, whose Unicode version changes
with the Python version.

  python3 scripts/generate_redaction_tables.py --write   rewrite the C++ file
  python3 scripts/generate_redaction_tables.py --check   fail if it is stale

The generated file ends with a SHA-256 of redaction-data.json as serialized by
the ECMAScript JSON rules (see json_stringify). The native code and its tests
rely on that exact digest, so the serialization must not be swapped for
json.dumps, which formats some numbers and characters differently.
"""
import hashlib
import json
import math
import os
import re
import sys
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REDACTION_PATH = ROOT / 'contracts/redaction-data.json'
WORDLIST_PATH = ROOT / 'contracts/bip39-english.json'
UNICODE_PATH = ROOT / 'contracts/redaction-unicode-data.json'
OUTPUT_PATH = ROOT / 'src/security/redaction_tables.cpp'

MAX_CODE_POINT = 0x10FFFF
UINT32_MAX = 0xFFFFFFFF

_ARRAY_INDEX = re.compile('0|[1-9][0-9]{0,9}')
_SHORT_ESCAPES = {'"': '\\"', '\\': '\\\\', '\b': '\\b', '\t': '\\t', '\n': '\\n',
                  '\f': '\\f', '\r': '\\r'}
_VERSION = re.compile('[0-9.]+')
_WORD = re.compile('[a-z]{3,8}')
_LETTER = re.compile('[A-Za-z]')
# The provenance line of a generated file. A line may end at any of the four
# line terminators, not only at "\n".
_LINE_END = '\n\r  '
_PROVENANCE = re.compile(
    rf'(?:^|(?<=[{_LINE_END}]))'
    r'(// Unicode [0-9.]+ normalization data of Node\.js) \(ICU [0-9.]+\)\.'
    rf'(?=[{_LINE_END}]|\Z)')
_NOT_SPACE = ('[^\t\n\v\f\r    -     　'
              '﻿]')
_RECORDED_VERSION = re.compile(f'Unicode ({_NOT_SPACE}+) normalization data')


def parse_json(source):
    """Parse strict JSON; Python would otherwise also accept NaN and Infinity."""
    def reject(name):
        raise ValueError(f'{name} is not valid JSON')
    return json.loads(source, parse_constant=reject)


def is_integer(value):
    """True for a JSON number with no fractional part (1 and 1.0 both count)."""
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return math.isfinite(to_double(value))
    return isinstance(value, float) and value.is_integer()


def to_double(value):
    """A JSON number as the double the native side and the digest see.

    Whole numbers beyond 2**53 round to the nearest double, and ones too large
    for a double become infinity, exactly as when the contract was authored.
    """
    try:
        return float(value)
    except OverflowError:
        return math.inf if value > 0 else -math.inf


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


def ranges(code_points):
    """Collapse sorted code points into inclusive [first, last] runs."""
    out = []
    for cp in code_points:
        if out and out[-1][1] + 1 == cp:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return out


def read_rules(source):
    rules = parse_json(source)
    if rules is None:
        raise TypeError("Cannot read properties of null (reading 'placeholder')")
    if (not isinstance(rules, dict) or not isinstance(rules.get('placeholder'), str) or
            not rules['placeholder'] or not isinstance(rules.get('invisible'), list) or
            not isinstance(rules.get('confusables'), list)):
        raise ValueError('invalid native redaction data')
    previous = -1
    for cp in rules['invisible']:
        if not is_integer(cp) or cp <= previous or cp > 0xFFFF:
            raise ValueError('invisible code points must be sorted, unique BMP integers')
        previous = cp
    previous = -1
    for mapping in rules['confusables']:
        if not isinstance(mapping, (list, str)):
            raise TypeError('confusable mapping is not iterable')
        source_cp = mapping[0] if len(mapping) > 0 else None
        letter = mapping[1] if len(mapping) > 1 else None
        if (not is_integer(source_cp) or source_cp <= previous or
                source_cp > MAX_CODE_POINT or not is_integer(letter) or
                not _LETTER.fullmatch(code_unit(letter))):
            raise ValueError('invalid confusable mapping')
        previous = source_cp
    return rules


def read_wordlist(source):
    words = parse_json(source)
    valid = isinstance(words, list) and len(words) == 2048
    if valid:
        for i, word in enumerate(words):
            if (not isinstance(word, str) or not _WORD.fullmatch(word) or
                    (i and words[i - 1] >= word)):
                valid = False
                break
    if not valid:
        raise ValueError('BIP-39 wordlist must contain 2048 sorted, unique words')
    return words


def _code_point(value):
    return (isinstance(value, int) and not isinstance(value, bool) and
            0 <= value <= MAX_CODE_POINT and not 0xD800 <= value <= 0xDFFF)


def read_unicode_tables(source):
    """Read and check the frozen Unicode normalization data.

    nfkdMappings     [code point, [its full compatibility decomposition]],
                     sorted by code point. Hangul syllables are left out: the
                     native code decomposes and composes them arithmetically.
    compositionPairs [first, second, composite] for every primary composite,
                     sorted by first then second.
    combiningRanks   [code point, rank] for every combining mark that can
                     reorder, sorted by code point. Ranks keep the order and
                     the equalities of the real canonical combining classes,
                     which is all reordering and composition blocking compare.
    """
    data = parse_json(source)
    error = ValueError('invalid frozen Unicode normalization data')
    if not isinstance(data, dict):
        raise error
    for name in ('unicodeVersion', 'icuVersion'):
        if not isinstance(data.get(name), str) or not _VERSION.fullmatch(data[name]):
            raise error
    mappings = data.get('nfkdMappings')
    pairs = data.get('compositionPairs')
    ranks = data.get('combiningRanks')
    if not all(isinstance(table, list) and table for table in (mappings, pairs, ranks)):
        raise error
    previous = -1
    for row in mappings:
        if (not isinstance(row, list) or len(row) != 2 or not _code_point(row[0]) or
                row[0] <= previous or not isinstance(row[1], list) or not row[1] or
                not all(_code_point(cp) for cp in row[1])):
            raise error
        previous = row[0]
    previous = (-1, -1)
    for row in pairs:
        if (not isinstance(row, list) or len(row) != 3 or
                not all(_code_point(cp) for cp in row) or tuple(row[:2]) <= previous):
            raise error
        previous = tuple(row[:2])
    previous = -1
    for row in ranks:
        if (not isinstance(row, list) or len(row) != 2 or not _code_point(row[0]) or
                row[0] <= previous or isinstance(row[1], bool) or
                not isinstance(row[1], int) or not 1 <= row[1] <= 255):
            raise error
        previous = row[0]
    return data


def hex_literal(cp):
    return f'0x{int(cp):04X}'


def code_unit(value):
    """The character for a whole number taken modulo 65536 (one UTF-16 unit)."""
    return chr(int(to_double(value)) & 0xFFFF)


def cpp_string(text):
    """A C++ string literal with every non-ASCII character as UTF-8 hex escapes.

    The empty "" after each character stops the last escape from swallowing a
    following hex digit. A character outside the Basic Multilingual Plane is
    written as two U+FFFD replacement characters (one per UTF-16 half), which
    is what the tables have always contained for such input.
    """
    out = []
    for char in quote_json_string(text):
        code = ord(char)
        if code < 0x80:
            out.append(char)
            continue
        encoded = [b'\xef\xbf\xbd'] * 2 if code > 0xFFFF else [char.encode('utf-8')]
        for part in encoded:
            out.append(''.join(f'\\x{byte:x}' for byte in part) + '""')
    return ''.join(out)


def render(words, rules, unicode_tables):
    unicode_version = unicode_tables['unicodeVersion']
    out = '// Generated by scripts/generate_redaction_tables.py from\n'
    out += '// contracts/redaction-data.json, bip39-english.json and the\n'
    out += (f'// Unicode {unicode_version} normalization data of Node.js '
            f"(ICU {unicode_tables['icuVersion']}).\n")
    out += '// Do not edit; run make generate-redaction-tables.\n'
    out += '#include "redaction_tables.h"\n\n'
    out += 'namespace fortis::security {\n\n'
    out += f'const char kRedactionUnicodeVersion[] = {quote_json_string(unicode_version)};\n'
    out += f"const char kRedactionPlaceholder[] = {cpp_string(rules['placeholder'])};\n\n"
    out += 'const char* const kBip39English[2048] = {\n'
    for i in range(0, len(words), 8):
        out += '  ' + ', '.join(f'"{word}"' for word in words[i:i + 8]) + ',\n'
    out += '};\n\n'
    out += 'const RedactionRange kRedactionInvisible[] = {\n'
    for first, last in ranges(rules['invisible']):
        out += f'  {{{hex_literal(first)}, {hex_literal(last)}}},\n'
    out += '};\n'
    out += ('const size_t kRedactionInvisibleCount = '
            'sizeof(kRedactionInvisible) / sizeof(kRedactionInvisible[0]);\n\n')
    out += 'const RedactionConfusable kRedactionConfusables[] = {\n'
    for mapping in rules['confusables']:
        out += f"  {{{hex_literal(mapping[0])}, '{code_unit(mapping[1])}'}},\n"
    out += '};\n'
    out += ('const size_t kRedactionConfusableCount = '
            'sizeof(kRedactionConfusables) / sizeof(kRedactionConfusables[0]);\n\n')
    pool = []
    lines = ['const NfkdMapping kNfkdMappings[] = {\n']
    for cp, mapped in unicode_tables['nfkdMappings']:
        lines.append(f'  {{{hex_literal(cp)}, {len(pool)}U, {len(mapped)}U}},\n')
        pool.extend(mapped)
    if len(pool) > 0xFFFF:
        raise ValueError('NFKD pool exceeds a 16-bit offset')
    lines.append('};\n')
    lines.append('const size_t kNfkdMappingCount = sizeof(kNfkdMappings) / sizeof(kNfkdMappings[0]);\n')
    lines.append('const uint32_t kNfkdPool[] = {\n')
    for i in range(0, len(pool), 10):
        lines.append('  ' + ', '.join(map(hex_literal, pool[i:i + 10])) + ',\n')
    lines.append('};\n\n')
    lines.append('const CompositionPair kCompositionPairs[] = {\n')
    for first, second, composite in unicode_tables['compositionPairs']:
        lines.append(f'  {{{hex_literal(first)}, {hex_literal(second)}, '
                     f'{hex_literal(composite)}}},\n')
    lines.append('};\n')
    lines.append('const size_t kCompositionPairCount = '
                 'sizeof(kCompositionPairs) / sizeof(kCompositionPairs[0]);\n\n')
    lines.append('const CombiningRank kCombiningRanks[] = {\n')
    ranks = unicode_tables['combiningRanks']
    for i in range(0, len(ranks), 6):
        lines.append('  ' + ', '.join(f'{{{hex_literal(cp)}, {rank}}}'
                                      for cp, rank in ranks[i:i + 6]) + ',\n')
    lines.append('};\n')
    lines.append('const size_t kCombiningRankCount = '
                 'sizeof(kCombiningRanks) / sizeof(kCombiningRanks[0]);\n\n')
    out += ''.join(lines)
    digest = hashlib.sha256(json_stringify(rules).encode('utf-8')).hexdigest()
    out += '// SHA-256 of native-owned redaction data.\n'
    out += f'const char kRedactionRulesSha256[] = "{digest}";\n\n'
    out += '}  // namespace fortis::security\n'
    return out


def same_generated_tables(current, generated):
    """Compare two generated files, ignoring the ICU build version.

    The ICU build version is provenance, not table content: identical Unicode
    data ships under different ICU patch versions. Every actual table byte and
    the Unicode version are still compared.
    """
    def normalize(text):
        return _PROVENANCE.sub(lambda match: match.group(1) + '.', text, count=1)
    return normalize(current) == normalize(generated)


def read_text(path):
    return path.read_bytes().decode('utf-8', 'replace')


def run(arguments):
    mode = arguments[0] if arguments else None
    if mode not in ('--write', '--check'):
        raise ValueError('usage: python3 scripts/generate_redaction_tables.py --write|--check')
    words = read_wordlist(read_text(WORDLIST_PATH))
    rules = read_rules(read_text(REDACTION_PATH))
    unicode_tables = read_unicode_tables(read_text(UNICODE_PATH))
    generated = render(words, rules, unicode_tables)
    if mode == '--write':
        OUTPUT_PATH.write_bytes(generated.encode('utf-8'))
        sys.stdout.write(f'generated {os.path.relpath(OUTPUT_PATH, ROOT)}\n')
        return 0
    try:
        current = read_text(OUTPUT_PATH)
    except OSError:
        current = ''
    if not same_generated_tables(current, generated):
        recorded = _RECORDED_VERSION.search(current)
        if recorded and recorded.group(1) != unicode_tables['unicodeVersion']:
            sys.stderr.write(
                f'redaction tables were generated with Unicode {recorded.group(1)}; '
                f"{os.path.relpath(UNICODE_PATH, ROOT)} has {unicode_tables['unicodeVersion']}\n")
        sys.stderr.write('native redaction tables are stale; run make generate-redaction-tables\n')
        return 1
    sys.stdout.write('native redaction tables match native contracts\n')
    return 0


def main(arguments):
    try:
        return run(arguments)
    except Exception as error:  # every failure is a one-line message and status 1
        sys.stderr.write(f'{error}\n')
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
