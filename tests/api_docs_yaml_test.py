#!/usr/bin/env python3
"""Checks scripts/api_docs_yaml.py, the YAML reader and writer behind the
generated API reference.

The served /openapi.yaml must not change shape when nothing changed in the
contract, so the writer's layout is pinned here: each case gives a source
document, the data it must read as (with keys in the order a JavaScript
object would hold them), and the exact text it must be written back as. The
expected text was produced by version 2 of the JavaScript "yaml" package,
whose layout the writer follows. Anything outside the supported subset must
be refused rather than guessed at; those inputs are listed at the end.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts'))

import api_docs_yaml  # noqa: E402

ROUND_TRIP_CASES = [
    ('scalar styles and spellings are kept',
     (
         'paths:\n'
         '  plain: some text\n'
         "  'single': 'it''s'\n"
         '  "double": "tab\\there"\n'
         '  flag: True\n'
         '  off: false\n'
         '  nothing: ~\n'
         '  none: null\n'
         '  count: 200\n'
         '  signed: +5\n'
         '  padded: 007\n'
         '  ratio: 1.50\n'
         '  whole: 1.0\n'
         '  negative: -1.0\n'
         '  bare: 2.\n'
         '  small: 0.0000001\n'
         '  big: 1000000000000000000\n'
         '  version: 3.0.3\n'
         "  ref: '#/components/schemas/X'\n"
     ),
     {'paths': {'plain': 'some text',
                'single': "it's",
                'double': 'tab\there',
                'flag': True,
                'off': False,
                'nothing': None,
                'none': None,
                'count': 200,
                'signed': 5,
                'padded': 7,
                'ratio': 1.5,
                'whole': 1,
                'negative': -1,
                'bare': 2,
                'small': 1e-07,
                'big': 1000000000000000000,
                'version': '3.0.3',
                'ref': '#/components/schemas/X'}},
     (
         'paths:\n'
         '  plain: some text\n'
         "  'single': 'it''s'\n"
         '  "double": "tab\\there"\n'
         '  flag: True\n'
         '  off: false\n'
         '  nothing: ~\n'
         '  none: null\n'
         '  count: 200\n'
         '  signed: 5\n'
         '  padded: 7\n'
         '  ratio: 1.50\n'
         '  whole: 1.0\n'
         '  negative: -1.0\n'
         '  bare: 2\n'
         '  small: 1e-7\n'
         '  big: 1000000000000000000\n'
         '  version: 3.0.3\n'
         "  ref: '#/components/schemas/X'\n"
     )),
    ('long scalars are folded at 80 columns',
     (
         'paths:\n'
         '  description: Read-only controller observations with UTC serving boundaries, uncertaint'
         'y, provenance and update progress. Does not activate releases.\n'
         '  nested:\n'
         '    deeper:\n'
         "      quoted: 'Read-only controller observations with UTC serving boundaries, uncertaint"
         "y, provenance and update progress. Does not activate releases. It''s Read-only controlle"
         'r observations with UTC serving boundaries, uncertainty, provenance and update progress.'
         " Does not activate releases.'\n"
         '      double: "`Read-only controller observations with UTC serving boundaries, uncertain'
         'ty, provenance and update progress. Does not activate releases.` \\"x\\" Read-only control'
         'ler observations with UTC serving boundaries, uncertainty, provenance and update progres'
         's. Does not activate releases."\n'
         '  unbreakable: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
         'xxxxxxxxxxxxxxxxxxxxxxxxxxx tail words here\n'
         '  a-rather-long-key-name-that-pushes-the-value-to-the-right-of-the-page-width-limit: Rea'
         'd-only controller observations with UTC serving boundaries, uncertainty, provenance and '
         'update progress. Does not activate releases.\n'
     ),
     {'paths': {'description': 'Read-only controller observations with UTC serving boundaries, '
                               'uncertainty, provenance and update progress. Does not activate '
                               'releases.',
                'nested': {'deeper': {'quoted': 'Read-only controller observations with UTC serving '
                                                'boundaries, uncertainty, provenance and update '
                                                "progress. Does not activate releases. It's "
                                                'Read-only controller observations with UTC serving '
                                                'boundaries, uncertainty, provenance and update '
                                                'progress. Does not activate releases.',
                                      'double': '`Read-only controller observations with UTC '
                                                'serving boundaries, uncertainty, provenance and '
                                                'update progress. Does not activate releases.` "x" '
                                                'Read-only controller observations with UTC serving '
                                                'boundaries, uncertainty, provenance and update '
                                                'progress. Does not activate releases.'}},
                'unbreakable': 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx '
                               'tail words here',
                'a-rather-long-key-name-that-pushes-the-value-to-the-right-of-the-page-width-limit': 'Read-only '
                                                                                                     'controller '
                                                                                                     'observations '
                                                                                                     'with '
                                                                                                     'UTC '
                                                                                                     'serving '
                                                                                                     'boundaries, '
                                                                                                     'uncertainty, '
                                                                                                     'provenance '
                                                                                                     'and '
                                                                                                     'update '
                                                                                                     'progress. '
                                                                                                     'Does '
                                                                                                     'not '
                                                                                                     'activate '
                                                                                                     'releases.'}},
     (
         'paths:\n'
         '  description: Read-only controller observations with UTC serving boundaries,\n'
         '    uncertainty, provenance and update progress. Does not activate releases.\n'
         '  nested:\n'
         '    deeper:\n'
         "      quoted: 'Read-only controller observations with UTC serving boundaries,\n"
         '        uncertainty, provenance and update progress. Does not activate releases.\n'
         "        It''s Read-only controller observations with UTC serving boundaries,\n"
         '        uncertainty, provenance and update progress. Does not activate\n'
         "        releases.'\n"
         '      double: "`Read-only controller observations with UTC serving boundaries,\n'
         '        uncertainty, provenance and update progress. Does not activate\n'
         '        releases.` \\"x\\" Read-only controller observations with UTC serving\n'
         '        boundaries, uncertainty, provenance and update progress. Does not\n'
         '        activate releases."\n'
         '  unbreakable: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
         'xxxxxxxxxxxxxxxxxxxxxxxxxxx\n'
         '    tail words here\n'
         '  a-rather-long-key-name-that-pushes-the-value-to-the-right-of-the-page-width-limit:\n'
         '    Read-only controller observations with UTC serving boundaries, uncertainty,\n'
         '    provenance and update progress. Does not activate releases.\n'
     )),
    ('line width counts UTF-16 units',
     (
         'paths:\n'
         '  text: \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \u2014 \xe9\xe9 end\n'
     ),
     {'paths': {'text': '😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 😀 — éé end'}},
     (
         'paths:\n'
         '  text: \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600\n'
         '    \U0001f600 \U0001f600 \U0001f600 \U0001f600 \U0001f600 \u2014 \xe9\xe9 end\n'
     )),
    ('flow collections are padded and broken when long',
     (
         'paths:\n'
         '  tags: [System]\n'
         '  empty: []\n'
         '  none: {}\n'
         '  schema: {type: integer, enum: [200, 503]}\n'
         '  required: [schemaVersion, generatedMs, status, healthy, orchestrator, node, extra, mor'
         'e]\n'
         '  deep:\n'
         '    deeper:\n'
         "      properties: { nullable: true, allOf: [{ $ref: '#/components/schemas/ReleaseDeploym"
         "ent' }, { type: object }], description: x }\n"
         '  quoted: [ \'a,b\', "c", d ]\n'
     ),
     {'paths': {'tags': ['System'],
                'empty': [],
                'none': {},
                'schema': {'type': 'integer', 'enum': [200, 503]},
                'required': ['schemaVersion',
                             'generatedMs',
                             'status',
                             'healthy',
                             'orchestrator',
                             'node',
                             'extra',
                             'more'],
                'deep': {'deeper': {'properties': {'nullable': True,
                                                   'allOf': [{'$ref': '#/components/schemas/ReleaseDeployment'},
                                                             {'type': 'object'}],
                                                   'description': 'x'}}},
                'quoted': ['a,b', 'c', 'd']}},
     (
         'paths:\n'
         '  tags: [ System ]\n'
         '  empty: []\n'
         '  none: {}\n'
         '  schema: { type: integer, enum: [ 200, 503 ] }\n'
         '  required:\n'
         '    [\n'
         '      schemaVersion,\n'
         '      generatedMs,\n'
         '      status,\n'
         '      healthy,\n'
         '      orchestrator,\n'
         '      node,\n'
         '      extra,\n'
         '      more\n'
         '    ]\n'
         '  deep:\n'
         '    deeper:\n'
         '      properties:\n'
         '        {\n'
         '          nullable: true,\n'
         "          allOf: [ { $ref: '#/components/schemas/ReleaseDeployment' }, { type: object } "
         '],\n'
         '          description: x\n'
         '        }\n'
         '  quoted: [ \'a,b\', "c", d ]\n'
     )),
    ('a comma splits plain text inside a flow mapping',
     (
         'paths:\n'
         '  disk: { type: integer, description: Used bytes, or allocated bytes when capacity is un'
         'available. }\n'
         '  set: { a, b }\n'
     ),
     {'paths': {'disk': {'type': 'integer',
                         'description': 'Used bytes',
                         'or allocated bytes when capacity is unavailable.': None},
                'set': {'a': None, 'b': None}}},
     (
         'paths:\n'
         '  disk:\n'
         '    {\n'
         '      type: integer,\n'
         '      description: Used bytes,\n'
         '      or allocated bytes when capacity is unavailable.\n'
         '    }\n'
         '  set: { a, b }\n'
     )),
    ('block scalars keep their style; folded text is folded again',
     (
         'paths:\n'
         '  literal: |\n'
         '    First line.\n'
         '\n'
         '    Second paragraph, kept exactly as written even when the line is much longer than eig'
         'hty columns in total.\n'
         '      indented\n'
         '  stripped: |-\n'
         '    no final newline\n'
         '  folded: >-\n'
         '    Version, runtime, uptime,\n'
         '    process CPU and memory.\n'
         '\n'
         '    A second paragraph that is long enough to need folding once it has been joined into '
         'a single line of text.\n'
         '      more indented line stays\n'
         '    back again\n'
         '  clipped: >\n'
         '    with a final\n'
         '    newline\n'
         '  items:\n'
         '    - |\n'
         '      in a sequence\n'
     ),
     {'paths': {'literal': 'First line.\n'
                           '\n'
                           'Second paragraph, kept exactly as written even when the line is much '
                           'longer than eighty columns in total.\n'
                           '  indented\n',
                'stripped': 'no final newline',
                'folded': 'Version, runtime, uptime, process CPU and memory.\n'
                          'A second paragraph that is long enough to need folding once it has been '
                          'joined into a single line of text.\n'
                          '  more indented line stays\n'
                          'back again',
                'clipped': 'with a final newline\n',
                'items': ['in a sequence\n']}},
     (
         'paths:\n'
         '  literal: |\n'
         '    First line.\n'
         '\n'
         '    Second paragraph, kept exactly as written even when the line is much longer than eig'
         'hty columns in total.\n'
         '      indented\n'
         '  stripped: |-\n'
         '    no final newline\n'
         '  folded: >-\n'
         '    Version, runtime, uptime, process CPU and memory.\n'
         '\n'
         '    A second paragraph that is long enough to need folding once it has been\n'
         '    joined into a single line of text.\n'
         '      more indented line stays\n'
         '    back again\n'
         '  clipped: >\n'
         '    with a final newline\n'
         '  items:\n'
         '    - |\n'
         '      in a sequence\n'
     )),
    ('blank lines are kept once, and dropped at the edges',
     (
         '\n'
         '\n'
         'paths:\n'
         '  a: 1\n'
         '\n'
         '\n'
         '  b:\n'
         '\n'
         '    c: 2\n'
         '    d: |\n'
         '      text\n'
         '\n'
         '  e:\n'
         '    - x\n'
         '\n'
         '    - y: 1\n'
         '      z: 2\n'
         '\n'
         'last: 1\n'
         '\n'
     ),
     {'paths': {'a': 1, 'b': {'c': 2, 'd': 'text\n'}, 'e': ['x', {'y': 1, 'z': 2}]}, 'last': 1},
     (
         'paths:\n'
         '  a: 1\n'
         '\n'
         '  b:\n'
         '\n'
         '    c: 2\n'
         '    d: |\n'
         '      text\n'
         '\n'
         '  e:\n'
         '    - x\n'
         '\n'
         '    - y: 1\n'
         '      z: 2\n'
         '\n'
         'last: 1\n'
     )),
    ('sequences are indented under their key',
     (
         'paths:\n'
         '  same:\n'
         '  - one\n'
         '  - two\n'
         '  maps:\n'
         '      - name: a\n'
         '        value: 1\n'
         '      - name: b\n'
         '  nested:\n'
         '    - - x\n'
         '      - y\n'
         '    -\n'
         '      k: v\n'
     ),
     {'paths': {'same': ['one', 'two'],
                'maps': [{'name': 'a', 'value': 1}, {'name': 'b'}],
                'nested': [['x', 'y'], {'k': 'v'}]}},
     (
         'paths:\n'
         '  same:\n'
         '    - one\n'
         '    - two\n'
         '  maps:\n'
         '    - name: a\n'
         '      value: 1\n'
         '    - name: b\n'
         '  nested:\n'
         '    - - x\n'
         '      - y\n'
         '    - k: v\n'
     )),
]

MERGE_CASES = [
    ('added data is written in block style with plain keys',
     (
         'paths:\n'
         '  /old:\n'
         '    get: { summary: Old }\n'
     ),
     [['/new',
       {'get': {'tags': ['System'],
                'summary': 'New',
                'security': [],
                'responses': {'404': {'description': 'Missing.'},
                              'default': {'description': 'Other.'},
                              '200': {'description': 'OK: fine'}}}}]],
     {'paths': {'/old': {'get': {'summary': 'Old'}},
                '/new': {'get': {'tags': ['System'],
                                 'summary': 'New',
                                 'security': [],
                                 'responses': {'200': {'description': 'OK: fine'},
                                               '404': {'description': 'Missing.'},
                                               'default': {'description': 'Other.'}}}}}},
     (
         'paths:\n'
         '  /old:\n'
         '    get: { summary: Old }\n'
         '  /new:\n'
         '    get:\n'
         '      tags:\n'
         '        - System\n'
         '      summary: New\n'
         '      security: []\n'
         '      responses:\n'
         '        "200":\n'
         '          description: "OK: fine"\n'
         '        "404":\n'
         '          description: Missing.\n'
         '        default:\n'
         '          description: Other.\n'
     )),
    ('replacing keeps the position and the way the key was written',
     (
         'paths:\n'
         "  '/a':\n"
         '    get: { summary: A }\n'
         '  /b:\n'
         '    get: { summary: B }\n'
     ),
     [['/a',
       {'post': {'summary': 'Replaced',
                 'deprecated': True,
                 'x-limit': 1.5,
                 'x-none': None,
                 'x-empty': {}}}]],
     {'paths': {'/a': {'post': {'summary': 'Replaced',
                                'deprecated': True,
                                'x-limit': 1.5,
                                'x-none': None,
                                'x-empty': {}}},
                '/b': {'get': {'summary': 'B'}}}},
     (
         'paths:\n'
         "  '/a':\n"
         '    post:\n'
         '      summary: Replaced\n'
         '      deprecated: true\n'
         '      x-limit: 1.5\n'
         '      x-none: null\n'
         '      x-empty: {}\n'
         '  /b:\n'
         '    get: { summary: B }\n'
     )),
    ('strings that would read as something else are quoted',
     (
         'paths:\n'
         '  /a: 1\n'
     ),
     [['/q',
       {'a': 'true',
        'b': '12',
        'c': '',
        'd': 'a: b',
        'e': '- x',
        'f': "it's: x",
        'g': 'say "hi": x',
        'h': '#tag',
        'i': 'null',
        'j': '1.5',
        'k': ' lead',
        'true': 'key',
        'x y': 'spaced key',
        '@at': 'indicator'}]],
     {'paths': {'/a': 1,
                '/q': {'a': 'true',
                       'b': '12',
                       'c': '',
                       'd': 'a: b',
                       'e': '- x',
                       'f': "it's: x",
                       'g': 'say "hi": x',
                       'h': '#tag',
                       'i': 'null',
                       'j': '1.5',
                       'k': ' lead',
                       'true': 'key',
                       'x y': 'spaced key',
                       '@at': 'indicator'}}},
     (
         'paths:\n'
         '  /a: 1\n'
         '  /q:\n'
         '    a: "true"\n'
         '    b: "12"\n'
         '    c: ""\n'
         '    d: "a: b"\n'
         '    e: "- x"\n'
         '    f: "it\'s: x"\n'
         '    g: \'say "hi": x\'\n'
         '    h: "#tag"\n'
         '    i: "null"\n'
         '    j: "1.5"\n'
         '    k: " lead"\n'
         '    "true": key\n'
         '    x y: spaced key\n'
         '    "@at": indicator\n'
     )),
    ('multi-line strings become block scalars',
     (
         'paths:\n'
         '  /a: 1\n'
     ),
     [['/m',
       {'short': 'one\ntwo\n',
        'strip': 'one\n\ntwo',
        'long': 'Read-only controller observations with UTC serving boundaries, uncertainty, '
                'provenance and update progress. Does not activate releases. Read-only controller '
                'observations with UTC serving boundaries, uncertainty, provenance and update '
                'progress. Does not activate releases.\n'
                'Second line.\n',
        'list': ['a\nb',
                 'Read-only controller observations with UTC serving boundaries, uncertainty, '
                 'provenance and update progress. Does not activate releases. Read-only controller '
                 'observations with UTC serving boundaries, uncertainty, provenance and update '
                 'progress. Does not activate releases.'],
        'control': 'bell\x07',
        'trailing': 'a \nb'}]],
     {'paths': {'/a': 1,
                '/m': {'short': 'one\ntwo\n',
                       'strip': 'one\n\ntwo',
                       'long': 'Read-only controller observations with UTC serving boundaries, '
                               'uncertainty, provenance and update progress. Does not activate '
                               'releases. Read-only controller observations with UTC serving '
                               'boundaries, uncertainty, provenance and update progress. Does not '
                               'activate releases.\n'
                               'Second line.\n',
                       'list': ['a\nb',
                                'Read-only controller observations with UTC serving boundaries, '
                                'uncertainty, provenance and update progress. Does not activate '
                                'releases. Read-only controller observations with UTC serving '
                                'boundaries, uncertainty, provenance and update progress. Does not '
                                'activate releases.'],
                       'control': 'bell\x07',
                       'trailing': 'a \nb'}}},
     (
         'paths:\n'
         '  /a: 1\n'
         '  /m:\n'
         '    short: |\n'
         '      one\n'
         '      two\n'
         '    strip: |-\n'
         '      one\n'
         '\n'
         '      two\n'
         '    long: >\n'
         '      Read-only controller observations with UTC serving boundaries,\n'
         '      uncertainty, provenance and update progress. Does not activate releases.\n'
         '      Read-only controller observations with UTC serving boundaries,\n'
         '      uncertainty, provenance and update progress. Does not activate releases.\n'
         '\n'
         '      Second line.\n'
         '    list:\n'
         '      - |-\n'
         '        a\n'
         '        b\n'
         '      - Read-only controller observations with UTC serving boundaries,\n'
         '        uncertainty, provenance and update progress. Does not activate releases.\n'
         '        Read-only controller observations with UTC serving boundaries,\n'
         '        uncertainty, provenance and update progress. Does not activate releases.\n'
         '    control: "bell\\a"\n'
         '    trailing: |-\n'
         '      a \n'
         '      b\n'
     )),
]

REFUSED_CASES = [
    ('tab', 'paths:\n\ta: 1\n'),
    ('carriage return', 'paths:\r\n  a: 1\r\n'),
    ('trailing whitespace', 'paths: \n  a: 1\n'),
    ('comment line', '# note\npaths:\n  a: 1\n'),
    ('trailing comment', 'paths:\n  a: 1 # note\n'),
    ('document marker', '---\npaths:\n  a: 1\n'),
    ('anchor', 'paths:\n  a: &x 1\n'),
    ('alias', 'paths:\n  a: *x\n'),
    ('tag', 'paths:\n  a: !!str 1\n'),
    ('multi-line plain scalar', 'paths:\n  a: one\n    two\n'),
    ('multi-line quoted scalar', "paths:\n  a: 'one\n    two'\n"),
    ('multi-line flow collection', 'paths:\n  a: [one,\n    two]\n'),
    ('value on the next line', 'paths:\n  a:\n    one\n'),
    ('explicit key', 'paths:\n  ? a\n  : 1\n'),
    ('empty value', 'paths:\n  a:\nb: 1\n'),
    ('empty flow value', 'paths:\n  a: { b: }\n'),
    ('duplicate key', 'paths:\n  a: 1\n  a: 2\n'),
    ('duplicate key in different quotes', 'paths:\n  a: { \'200\': 1, "200": 2 }\n'),
    ('number as key', 'paths:\n  200: ok\n'),
    ('boolean as key', 'paths:\n  true: ok\n'),
    ('hexadecimal', 'paths:\n  a: 0x1F\n'),
    ('octal', 'paths:\n  a: 0o17\n'),
    ('exponent', 'paths:\n  a: 1e3\n'),
    ('infinity', 'paths:\n  a: .inf\n'),
    ('negative zero', 'paths:\n  a: -0\n'),
    ('inexact integer', 'paths:\n  a: 12345678901234567890\n'),
    ('nested mapping on one line', 'paths:\n  a: b: c\n'),
    ('pair in flow sequence', 'paths:\n  a: [ b: c ]\n'),
    ('trailing comma', 'paths:\n  a: [ b, ]\n'),
    ('keep chomping', 'paths:\n  a: |+\n    text\n'),
    ('indentation indicator', 'paths:\n  a: |2\n    text\n'),
    ('block scalar starting with a blank line', 'paths:\n  a: |\n\n    text\n'),
    ('unsupported escape', 'paths:\n  a: "\\e"\n'),
    ('text after a quoted value', "paths:\n  a: 'x' y\n"),
    ('indicator at start of plain value', 'paths:\n  a: @x\n'),
    ('root sequence', '- a\n- b\n'),
    ('bad indentation', 'paths:\n    a: 1\n  b: 2\n'),
    ('space before colon', 'paths:\n  a : 1\n'),
]


def same_data(actual, expected):
    """Equal values, with mappings also equal in key order. A float and an
    integer of the same value are the same number, as they are in JSON."""
    if isinstance(expected, dict):
        return (isinstance(actual, dict) and list(actual) == list(expected)
                and all(same_data(actual[key], expected[key]) for key in expected))
    if isinstance(expected, list):
        return (isinstance(actual, list) and len(actual) == len(expected)
                and all(same_data(a, e) for a, e in zip(actual, expected)))
    if expected is None or isinstance(expected, (bool, str)):
        return type(actual) is type(expected) and actual == expected
    return isinstance(actual, (int, float)) and not isinstance(actual, bool) and actual == expected


def in_js_key_order(value):
    if isinstance(value, dict):
        return {key: in_js_key_order(value[key]) for key in api_docs_yaml.js_key_order(list(value))}
    if isinstance(value, list):
        return [in_js_key_order(item) for item in value]
    return value


class ReaderAndWriterTest(unittest.TestCase):
    def test_round_trip(self):
        for name, source, data, text in ROUND_TRIP_CASES:
            with self.subTest(name):
                root = api_docs_yaml.parse_document(source, name)
                self.assertTrue(same_data(api_docs_yaml.to_js(root), data), api_docs_yaml.to_js(root))
                self.assertEqual(api_docs_yaml.emit_document(root), text)

    def test_written_text_is_stable(self):
        # Reading what was written and writing it again changes nothing, for
        # the cases whose output stays within the reader's one-line subset.
        stable = 0
        for name, source, _, text in ROUND_TRIP_CASES:
            try:
                root = api_docs_yaml.parse_document(text, name)
            except api_docs_yaml.YamlSubsetError:
                continue
            stable += 1
            self.assertEqual(api_docs_yaml.emit_document(root), text, name)
        self.assertGreaterEqual(stable, 3)

    def test_merged_data(self):
        for name, source, additions, data, text in MERGE_CASES:
            with self.subTest(name):
                root = api_docs_yaml.parse_document(source, name)
                paths = root.find('paths').value
                for key, value in additions:
                    node = api_docs_yaml.from_js(in_js_key_order(value))
                    existing = paths.find(key)
                    if existing is not None:
                        existing.value = node
                    else:
                        paths.entries.append(api_docs_yaml.Entry(api_docs_yaml.Scalar(key), node))
                self.assertTrue(same_data(api_docs_yaml.to_js(root), data), api_docs_yaml.to_js(root))
                self.assertEqual(api_docs_yaml.emit_document(root), text)

    def test_emptied_mapping(self):
        root = api_docs_yaml.parse_document('paths:\n  a: 1\n\n  b: 2\nlast: [x]\n', 'case')
        root.find('paths').value.entries.clear()
        root.find('last').value.items.clear()
        self.assertEqual(api_docs_yaml.emit_document(root), 'paths: {}\nlast: []\n')

    def test_refusals(self):
        for name, source in REFUSED_CASES:
            with self.subTest(name):
                with self.assertRaises(api_docs_yaml.YamlSubsetError) as raised:
                    api_docs_yaml.parse_document(source, 'contract.yaml')
                self.assertRegex(str(raised.exception), r'^contract\.yaml:\d+: ')

    def test_comments_only_where_allowed(self):
        source = '# heading\npaths: # why\n  # note\n  a: 1 # one\n  b: [x] # list\n  c: "#not a comment"\n'
        with self.assertRaises(api_docs_yaml.YamlSubsetError):
            api_docs_yaml.parse_document(source, 'base')
        root = api_docs_yaml.parse_document(source, 'overlay', allow_comments=True)
        self.assertEqual(api_docs_yaml.to_js(root),
                         {'paths': {'a': 1, 'b': ['x'], 'c': '#not a comment'}})

    def test_unwritable_values_are_refused(self):
        for value in ('ends with blank lines\n\n', ' starts with a space\nand continues'):
            with self.subTest(value):
                root = api_docs_yaml.from_js({'paths': {'text': value}})
                with self.assertRaises(api_docs_yaml.YamlSubsetError):
                    api_docs_yaml.emit_document(root)
        with self.assertRaises(api_docs_yaml.YamlSubsetError):
            api_docs_yaml.from_js({'paths': {'when': object()}})


class JavaScriptConventionsTest(unittest.TestCase):
    def test_number_text(self):
        for value, text in [
                (0, '0'), (-7, '-7'), (2 ** 53 - 1, '9007199254740991'), (10 ** 18, '1000000000000000000'),
                (1.0, '1'), (-1.0, '-1'), (0.0, '0'), (1.5, '1.5'), (0.1, '0.1'), (100.25, '100.25'),
                (0.000001, '0.000001'), (0.0000001, '1e-7'), (1.5e-10, '1.5e-10'), (1e20, '100000000000000000000'),
                (1e21, '1e+21'), (1.5e300, '1.5e+300'), (5e-324, '5e-324'),
                (123456789012345680000.0, '123456789012345680000')]:
            with self.subTest(value):
                self.assertEqual(api_docs_yaml.js_number_text(value), text)
        for value in (2 ** 60, 12345678901234567890, float('inf'), float('nan'), True, '1'):
            with self.subTest(value):
                with self.assertRaises(api_docs_yaml.YamlSubsetError):
                    api_docs_yaml.js_number_text(value)

    def test_key_order(self):
        self.assertEqual(api_docs_yaml.js_key_order(['default', '404', 'b', '200', '01', '-1', 'a', '4294967295', '7']),
                         ['7', '200', '404', 'default', 'b', '01', '-1', 'a', '4294967295'])

    def test_json_quote(self):
        self.assertEqual(api_docs_yaml.json_quote('a"b\\c\n\t\x07\x7f é —   \U0001F600'),
                         '"a\\"b\\\\c\\n\\t\\u0007\x7f é —   \U0001F600"')


if __name__ == '__main__':
    unittest.main()
