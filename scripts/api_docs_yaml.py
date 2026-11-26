"""Strict reader and writer for the YAML subset used by the API contract.

The API contract (contracts/openapi.base.yaml) and its native overlay
(openapi.native.yaml) are YAML, and Python has no YAML library in its standard
library. Rather than add a dependency to every build machine, this module
implements exactly the part of YAML those two files use, and refuses
everything else with an error naming the file and line. It never guesses.

Accepted input
  * block mappings and block sequences, indented with spaces;
  * flow collections ([a, b] and { a: 1 }) that open and close on one line;
  * plain, 'single-quoted' and "double-quoted" scalars on one line;
  * block scalars introduced by |, |-, > or >-;
  * blank lines, and (only where the caller allows them) # comments.
Refused, among others: tabs, anchors and aliases, tags, multi-line plain or
quoted scalars, multi-line flow collections, "? " keys, document markers,
keys that are not strings, empty values, duplicate keys, and number forms
whose text could not be reproduced exactly (hexadecimal, octal, exponents,
infinities, negative zero, integers beyond 2**53).

Output
  emit_document() writes a document back as text. The contract is served
  publicly at /openapi.yaml, so the text is kept stable: scalars keep the
  quoting style they were written with, long lines are folded at 80 columns,
  and collections keep their block or flow style. The layout rules are those
  of the JavaScript "yaml" package (version 2), which produced this text
  before; the folding and quoting functions below follow it rule for rule so
  that regenerating the file does not rewrite it.

Values follow JavaScript conventions where they are visible in the output:
numbers are written as JavaScript writes them, and to_js() orders mapping
keys the way a JavaScript object would (see js_key_order).
"""

import decimal
import re

PLAIN = 'PLAIN'
SINGLE_QUOTED = 'QUOTE_SINGLE'
DOUBLE_QUOTED = 'QUOTE_DOUBLE'
LITERAL_BLOCK = 'BLOCK_LITERAL'
FOLDED_BLOCK = 'BLOCK_FOLDED'

LINE_WIDTH = 80
MIN_CONTENT_WIDTH = 20
INDENT_STEP = '  '
MAX_SAFE_INTEGER = 2 ** 53 - 1


class YamlSubsetError(ValueError):
    """The input is outside the supported subset, or cannot be written."""


class Scalar:
    """A string, number, boolean or null, with how it was written.

    style is None for values that did not come from a file. source keeps the
    spelling of booleans and nulls (True, ~) and min_fraction_digits the
    trailing zeros of a decimal (1.50) so they are written back unchanged.
    """

    def __init__(self, value, style=None, source=None, min_fraction_digits=0):
        self.value = value
        self.style = style
        self.source = source
        self.min_fraction_digits = min_fraction_digits
        self.space_before = False  # a blank line precedes this node


class Entry:
    """One key of a mapping. value is None for a bare key in a flow mapping
    ({ a, b }), which YAML reads as a key whose value is null."""

    def __init__(self, key, value, space_before=False):
        self.key = key
        self.value = value
        self.space_before = space_before


class Mapping:
    def __init__(self, entries=None, flow=False):
        self.entries = entries if entries is not None else []
        self.flow = flow  # True: written as { ... }; otherwise block style
        self.space_before = False

    def find(self, key):
        for entry in self.entries:
            if entry.key.value == key:
                return entry
        return None


class Sequence:
    def __init__(self, items=None, flow=False):
        self.items = items if items is not None else []
        self.flow = flow
        self.space_before = False


# --------------------------------------------------------------------------
# JavaScript conventions
# --------------------------------------------------------------------------

_ARRAY_INDEX = re.compile(r'0|[1-9][0-9]*')


def js_key_order(keys):
    """Order keys as a JavaScript object enumerates them: keys that look like
    array indices ("200", "404") first in ascending numeric order, then the
    rest in the order written. The HTML reference and the overlay were always
    produced through JavaScript objects, so this order is part of the output.
    """
    numeric = [k for k in keys if _ARRAY_INDEX.fullmatch(k) and int(k) < 2 ** 32 - 1]
    numeric_set = set(numeric)
    return sorted(numeric, key=int) + [k for k in keys if k not in numeric_set]


def js_number_text(value):
    """Write a number as JavaScript's String(number) does (1.0 -> "1",
    1e21 -> "1e+21", 0.0000001 -> "1e-7")."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise YamlSubsetError(f'not a number: {value!r}')
    if isinstance(value, int):
        if abs(value) <= MAX_SAFE_INTEGER:
            return str(value)
        # JavaScript numbers are doubles. A larger integer is accepted only
        # if it survives that exactly and is written back digit for digit
        # (10**18 does, 2**60 does not).
        try:
            text = js_number_text(float(value))
        except OverflowError:
            text = None
        if text != str(value):
            raise YamlSubsetError(f'integer {value} cannot be represented exactly')
        return text
    if value != value or value in (float('inf'), float('-inf')):
        raise YamlSubsetError('infinite and not-a-number values are not supported')
    if value == 0:
        return '0'
    sign = '-' if value < 0 else ''
    # repr() gives the shortest digits that round-trip, as JavaScript does.
    _, digit_tuple, exponent = decimal.Decimal(repr(abs(value))).as_tuple()
    digits = ''.join(map(str, digit_tuple)).rstrip('0')
    exponent += len(digit_tuple) - len(digits)
    point = len(digits) + exponent  # value = 0.digits * 10**point
    if len(digits) <= point <= 21:
        return sign + digits + '0' * (point - len(digits))
    if 0 < point <= 21:
        return sign + digits[:point] + '.' + digits[point:]
    if -6 < point <= 0:
        return sign + '0.' + '0' * -point + digits
    mantissa = digits[0] + ('.' + digits[1:] if len(digits) > 1 else '')
    return f'{sign}{mantissa}e{"+" if point > 0 else "-"}{abs(point - 1)}'


_JSON_ESCAPES = {'"': '\\"', '\\': '\\\\', '\b': '\\b', '\f': '\\f',
                 '\n': '\\n', '\r': '\\r', '\t': '\\t'}
_JSON_ESCAPED = re.compile(r'["\\\x00-\x1f]')


def json_quote(text):
    """Quote a string as JavaScript's JSON.stringify does: only quotes,
    backslashes and control characters are escaped; non-ASCII is kept."""
    return '"' + _JSON_ESCAPED.sub(
        lambda m: _JSON_ESCAPES.get(m.group(), f'\\u{ord(m.group()):04x}'), text) + '"'


def to_js(node):
    """Turn a node tree into plain dict/list/str/int/float/bool/None, with
    mapping keys in JavaScript object order."""
    if isinstance(node, Scalar):
        return node.value
    if isinstance(node, Sequence):
        return [to_js(item) for item in node.items]
    values = {entry.key.value: None if entry.value is None else to_js(entry.value)
              for entry in node.entries}
    return {key: values[key] for key in js_key_order(list(values))}


def from_js(value):
    """Wrap plain data in nodes that carry no style of their own; the writer
    then chooses block collections and the plainest safe scalar form."""
    if isinstance(value, dict):
        entries = []
        for key, item in value.items():
            if not isinstance(key, str) or key == '':
                raise YamlSubsetError(f'mapping key must be a non-empty string: {key!r}')
            entries.append(Entry(Scalar(key), from_js(item)))
        return Mapping(entries)
    if isinstance(value, list):
        return Sequence([from_js(item) for item in value])
    if value is None or isinstance(value, (str, bool, int, float)):
        return Scalar(value)
    raise YamlSubsetError(f'unsupported value: {value!r}')


# --------------------------------------------------------------------------
# Reader
# --------------------------------------------------------------------------

_NULL = re.compile(r'~|[Nn]ull|NULL')
_BOOL = re.compile(r'[Tt]rue|TRUE|[Ff]alse|FALSE')
_INT = re.compile(r'[-+]?[0-9]+')
_FLOAT = re.compile(r'[-+]?(?:\.[0-9]+|[0-9]+\.[0-9]*)')
_UNSUPPORTED_NUMBER = re.compile(
    r'0o[0-7]+|0x[0-9a-fA-F]+|[-+]?\.(?:inf|Inf|INF)|\.nan|\.NaN|\.NAN|'
    r'[-+]?(?:\.[0-9]+|[0-9]+(?:\.[0-9]*)?)[eE][-+]?[0-9]+')
_KEY_END = re.compile(r':(?= |$)')
_BLOCK_HEADER = re.compile(r'([|>])(-?)')
_FORBIDDEN_CHARACTER = re.compile(r'[\x00-\x09\x0b-\x1f\x7f-\x9f﻿\ud800-\udfff  ]')
_DOUBLE_QUOTE_ESCAPES = {'\\': '\\', '"': '"', '/': '/', 'n': '\n', 't': '\t'}
# Characters that cannot start a plain scalar; "-" is allowed before a
# non-space ("-1", "-x"), "?" and ":" are refused outright for simplicity.
_INDICATORS = ',[]{}#&*!|>\'"%@`?:'


class _Line:
    def __init__(self, indent, text, number, blank_before):
        self.indent = indent
        self.text = text
        self.number = number
        self.blank_before = blank_before


def _is_sequence_item(text):
    return text == '-' or text.startswith('- ')


class _Reader:
    def __init__(self, text, name, allow_comments):
        self.name = name
        self.allow_comments = allow_comments
        self.lines = text.split('\n')
        self.position = 0  # index of the next unread physical line
        self.pending = None  # rest of a "- key: value" line, read as its own line
        for number, line in enumerate(self.lines, 1):
            bad = _FORBIDDEN_CHARACTER.search(line)
            if bad:
                self.fail(number, f'unsupported character U+{ord(bad.group()):04X} '
                                  '(tabs, carriage returns and control characters are refused)')
            if line.endswith(' '):
                self.fail(number, 'trailing whitespace')

    def fail(self, number, message):
        raise YamlSubsetError(f'{self.name}:{number}: {message}')

    # -- lines ------------------------------------------------------------

    def peek(self):
        """The next line that carries content, without consuming it."""
        if self.pending is not None:
            return self.pending
        blank = False
        index = self.position
        while index < len(self.lines):
            raw = self.lines[index]
            text = raw.lstrip(' ')
            if text == '':
                blank = True
            elif text.startswith('#'):
                if not self.allow_comments:
                    self.fail(index + 1, 'comments are not supported in this file')
            else:
                indent = len(raw) - len(text)
                if indent == 0 and (text.startswith('---') or text.startswith('...')):
                    self.fail(index + 1, 'document markers are not supported')
                self._peeked_index = index
                return _Line(indent, text, index + 1, blank)
            index += 1
        return None

    def consume(self):
        if self.pending is not None:
            self.pending = None
        else:
            self.position = self._peeked_index + 1

    # -- block structure --------------------------------------------------

    def read_document(self):
        first = self.peek()
        if first is None:
            self.fail(1, 'the document is empty')
        if first.indent != 0:
            self.fail(first.number, 'the document must start at column 1')
        if self._split_key(first) is None:
            self.fail(first.number, 'the document must be a mapping')
        root = self._read_mapping(0)
        root.space_before = False  # blank lines before the document are dropped
        rest = self.peek()
        if rest is not None:
            self.fail(rest.number, 'unexpected content after the document')
        return root

    def _read_collection(self, indent):
        line = self.peek()
        if _is_sequence_item(line.text):
            return self._read_sequence(indent)
        if self._split_key(line) is None:
            self.fail(line.number, 'expected "key: value" or "- item" '
                                   '(values that continue on a following line are not supported)')
        return self._read_mapping(indent)

    def _read_mapping(self, indent):
        mapping = Mapping()
        seen = set()
        while True:
            line = self.peek()
            if line is None or line.indent < indent:
                break
            if line.indent > indent:
                self.fail(line.number, 'unexpected indentation '
                                       '(values that continue on a following line are not supported)')
            split = None if _is_sequence_item(line.text) else self._split_key(line)
            if split is None:
                self.fail(line.number, 'expected "key: value"')
            key, rest = split
            self.consume()
            if key.value in seen:
                self.fail(line.number, f'duplicate key {key.value!r}')
            seen.add(key.value)
            first = not mapping.entries
            if first and line.blank_before:
                mapping.space_before = True
            if rest == '':
                value = self._read_nested_value(line, indent)
            else:
                value = self._read_inline_value(rest, line, indent)
            mapping.entries.append(Entry(key, value, line.blank_before and not first))
        return mapping

    def _read_nested_value(self, key_line, indent):
        following = self.peek()
        if following is not None and following.indent > indent:
            return self._read_collection(following.indent)
        if following is not None and following.indent == indent and _is_sequence_item(following.text):
            return self._read_sequence(indent)
        self.fail(key_line.number, 'empty value; write null explicitly')

    def _read_sequence(self, indent):
        sequence = Sequence()
        while True:
            line = self.peek()
            if line is None or line.indent < indent:
                break
            if line.indent > indent:
                self.fail(line.number, 'unexpected indentation '
                                       '(values that continue on a following line are not supported)')
            if not _is_sequence_item(line.text):
                break
            self.consume()
            if line.text == '-':
                following = self.peek()
                if following is None or following.indent <= indent:
                    self.fail(line.number, 'empty sequence item; write null explicitly')
                if following.blank_before:
                    self.fail(following.number, 'blank line after "-" is not supported')
                item = self._read_collection(following.indent)
            else:
                rest = line.text[2:]
                if rest.startswith(' '):
                    self.fail(line.number, 'use exactly one space after "-"')
                column = line.indent + 2
                nested = _Line(column, rest, line.number, False)
                if _is_sequence_item(rest) or self._split_key(nested) is not None:
                    # "- key: value" starts a mapping whose entries line up
                    # with the first key; "- - x" a nested sequence.
                    self.pending = nested
                    item = self._read_collection(column)
                else:
                    item = self._read_inline_value(rest, line, indent)
            first = not sequence.items
            if first and line.blank_before:
                sequence.space_before = True
            item.space_before = line.blank_before and not first
            sequence.items.append(item)
        return sequence

    # -- keys and values on one line --------------------------------------

    def _split_key(self, line):
        """Split "key: rest" into (key scalar, rest); None if the line is not
        a mapping entry."""
        text = line.text
        if text[0] in '\'"':
            key, end = self._read_quoted(text, 0, line)
            after = text[end:]
            if after == ':' or after.startswith(': '):
                return key, self._after_colon(after[1:], line)
            return None
        if text[0] in '[{':
            return None
        match = _KEY_END.search(text)
        comment = text.find(' #')
        if match is None or (comment != -1 and comment < match.start()):
            return None
        if text[match.start() - 1:match.start()] == ' ':
            self.fail(line.number, 'remove the space before ":"')
        key = self._plain_scalar(text[:match.start()], line)
        self._require_string_key(key, line)
        return key, self._after_colon(text[match.end():], line)

    def _after_colon(self, rest, line):
        """The value text after "key:"; empty when only a comment follows."""
        rest = rest.lstrip(' ')
        if rest.startswith('#'):
            if not self.allow_comments:
                self.fail(line.number, 'comments are not supported in this file')
            return ''
        return rest

    def _require_string_key(self, key, line):
        if not isinstance(key.value, str):
            self.fail(line.number, f'key {key.source or key.value!r} is not a string; quote it')
        if key.value == '__proto__':
            self.fail(line.number, 'the key __proto__ is not supported')

    def _read_inline_value(self, text, line, parent_indent):
        first = text[0]
        if first in '|>':
            header = self._without_comment(text, line)
            match = _BLOCK_HEADER.fullmatch(header)
            if match is None:
                self.fail(line.number, f'unsupported block scalar header {header!r} '
                                       '(only |, |-, > and >- are supported)')
            return self._read_block_scalar(match.group(1) == '|', match.group(2) == '-',
                                           line, parent_indent)
        if first in '&*!':
            self.fail(line.number, 'anchors, aliases and tags are not supported')
        if first in '[{' or first in '\'"':
            if first in '[{':
                node, end = self._read_flow(text, 0, line)
            else:
                node, end = self._read_quoted(text, 0, line)
            rest = text[end:]
            if rest.strip(' ') != '' and not (rest.startswith(' #') and self.allow_comments):
                self.fail(line.number, f'unexpected text after value: {rest.strip()!r}')
            return node
        text = self._without_comment(text, line)
        if _KEY_END.search(text):
            self.fail(line.number, 'a plain value cannot contain ": " or end with ":"; quote it')
        return self._plain_scalar(text, line)

    def _without_comment(self, text, line):
        comment = text.find(' #')
        if comment == -1:
            return text
        if not self.allow_comments:
            self.fail(line.number, '" #" starts a comment, and comments are not supported '
                                   'in this file; quote the value if it is text')
        return text[:comment].rstrip(' ')

    def _plain_scalar(self, text, line):
        """Check and interpret an unquoted scalar (YAML 1.2 core schema)."""
        if text == '':
            self.fail(line.number, 'empty value; write null explicitly')
        if text[0] in _INDICATORS or text == '-' or text.startswith('- '):
            self.fail(line.number, f'a plain scalar cannot start like {text[:12]!r}; quote it')
        if _NULL.fullmatch(text):
            return Scalar(None, PLAIN, source=text)
        if _BOOL.fullmatch(text):
            return Scalar(text[0] in 'tT', PLAIN, source=text)
        if _UNSUPPORTED_NUMBER.fullmatch(text):
            self.fail(line.number, f'unsupported number form {text!r}; write a plain decimal')
        if _INT.fullmatch(text):
            value = int(text)
            try:
                js_number_text(value)
            except YamlSubsetError:
                self.fail(line.number, f'integer {text} is too large to be exact; quote it')
            if value == 0 and text.startswith('-'):
                self.fail(line.number, 'negative zero is not supported')
            return Scalar(value, PLAIN, source=text)
        if _FLOAT.fullmatch(text):
            value = float(text)
            if value == 0 and text.startswith('-'):
                self.fail(line.number, 'negative zero is not supported')
            if value in (float('inf'), float('-inf')):
                self.fail(line.number, f'number {text} is out of range')
            digits = len(text) - text.index('.') - 1 if text.endswith('0') else 0
            return Scalar(value, PLAIN, source=text, min_fraction_digits=digits)
        return Scalar(text, PLAIN)

    def _read_quoted(self, text, start, line):
        """Read a quoted scalar starting at text[start]; return it and the
        index just past its closing quote."""
        quote = text[start]
        parts = []
        index = start + 1
        while index < len(text):
            char = text[index]
            if quote == "'":
                if char != "'":
                    parts.append(char)
                elif text[index + 1:index + 2] == "'":
                    parts.append("'")
                    index += 1
                else:
                    return Scalar(''.join(parts), SINGLE_QUOTED), index + 1
            elif char == '"':
                return Scalar(''.join(parts), DOUBLE_QUOTED), index + 1
            elif char == '\\':
                escape = text[index + 1:index + 2]
                if escape in _DOUBLE_QUOTE_ESCAPES:
                    parts.append(_DOUBLE_QUOTE_ESCAPES[escape])
                    index += 1
                elif escape in ('x', 'u'):
                    width = 2 if escape == 'x' else 4
                    code = text[index + 2:index + 2 + width]
                    if not re.fullmatch(r'[0-9a-fA-F]{%d}' % width, code):
                        self.fail(line.number, f'malformed \\{escape} escape')
                    if 0xD800 <= int(code, 16) <= 0xDFFF:
                        self.fail(line.number, 'surrogate escapes are not supported')
                    parts.append(chr(int(code, 16)))
                    index += 1 + width
                else:
                    self.fail(line.number, f'unsupported escape \\{escape}')
            else:
                parts.append(char)
            index += 1
        self.fail(line.number, 'quoted scalar does not end on its line '
                               '(multi-line quoted scalars are not supported)')

    # -- flow collections -------------------------------------------------

    def _skip_spaces(self, text, index):
        while index < len(text) and text[index] == ' ':
            index += 1
        return index

    def _flow_char(self, text, index, line):
        if index >= len(text):
            self.fail(line.number, 'flow collection does not end on its line '
                                   '(multi-line flow collections are not supported)')
        return text[index]

    def _read_flow(self, text, index, line):
        """Read a flow node starting at text[index]; return (node, end)."""
        char = self._flow_char(text, index, line)
        if char == '[':
            sequence = Sequence(flow=True)
            index = self._skip_spaces(text, index + 1)
            if self._flow_char(text, index, line) == ']':
                return sequence, index + 1
            while True:
                item, index = self._read_flow(text, index, line)
                sequence.items.append(item)
                index = self._skip_spaces(text, index)
                char = self._flow_char(text, index, line)
                if char == ']':
                    return sequence, index + 1
                if char != ',':
                    self.fail(line.number, f'expected "," or "]" in flow sequence, found {char!r} '
                                           '(key: value pairs need { } around them)')
                index = self._skip_spaces(text, index + 1)
                if self._flow_char(text, index, line) == ']':
                    self.fail(line.number, 'trailing comma in flow sequence')
        if char == '{':
            mapping = Mapping(flow=True)
            seen = set()
            index = self._skip_spaces(text, index + 1)
            if self._flow_char(text, index, line) == '}':
                return mapping, index + 1
            while True:
                if self._flow_char(text, index, line) in '[{':
                    self.fail(line.number, 'a collection cannot be a mapping key')
                key, index = self._read_flow(text, index, line)
                self._require_string_key(key, line)
                if key.value in seen:
                    self.fail(line.number, f'duplicate key {key.value!r}')
                seen.add(key.value)
                index = self._skip_spaces(text, index)
                char = self._flow_char(text, index, line)
                value = None
                if char == ':':
                    if text[index + 1:index + 2] != ' ':
                        self.fail(line.number, 'put a space after ":" in a flow mapping')
                    index = self._skip_spaces(text, index + 1)
                    if self._flow_char(text, index, line) in ',}':
                        self.fail(line.number, 'empty value; write null explicitly')
                    value, index = self._read_flow(text, index, line)
                    index = self._skip_spaces(text, index)
                    char = self._flow_char(text, index, line)
                mapping.entries.append(Entry(key, value))
                if char == '}':
                    return mapping, index + 1
                if char != ',':
                    self.fail(line.number, f'expected "," or "}}" in flow mapping, found {char!r}')
                index = self._skip_spaces(text, index + 1)
                if self._flow_char(text, index, line) == '}':
                    self.fail(line.number, 'trailing comma in flow mapping')
        if char in '\'"':
            return self._read_quoted(text, index, line)
        if char in '&*!':
            self.fail(line.number, 'anchors, aliases and tags are not supported')
        # A plain scalar inside a flow collection ends at a flow indicator or
        # at ": "; a comma therefore splits text, exactly as YAML specifies.
        end = index
        while end < len(text):
            char = text[end]
            if char in ',[]{}':
                break
            if char == ':' and text[end + 1:end + 2] in (' ', ',', ']', '}', ''):
                break
            if char == '#' and text[end - 1] == ' ':
                self.fail(line.number, 'comments inside flow collections are not supported')
            end += 1
        return self._plain_scalar(text[index:end].rstrip(' '), line), end

    # -- block scalars ----------------------------------------------------

    def _read_block_scalar(self, literal, strip, header_line, parent_indent):
        """Read the indented text below "key: |" or "key: >-"."""
        content = []
        block_indent = None
        index = self.position
        end = index
        while index < len(self.lines):
            raw = self.lines[index]
            if raw == '':
                content.append('')
            else:
                indent = len(raw) - len(raw.lstrip(' '))
                if indent <= parent_indent:
                    break
                if block_indent is None:
                    if content:
                        self.fail(index + 1, 'a block scalar cannot start with a blank line')
                    block_indent = indent
                elif indent < block_indent:
                    self.fail(index + 1, 'block scalar line is indented less than its first line')
                content.append(raw[block_indent:])
                end = index + 1
            index += 1
        if block_indent is None:
            self.fail(header_line.number, 'empty block scalar')
        # Blank lines after the last text line are not part of the value;
        # they are left unread and count as a blank line before what follows.
        content = content[:end - self.position]
        self.position = end
        if literal:
            value = '\n'.join(content)
        else:
            value = self._fold(content)
        return Scalar(value if strip else value + '\n', LITERAL_BLOCK if literal else FOLDED_BLOCK)

    @staticmethod
    def _fold(lines):
        """Join the lines of a folded (>) scalar: a single line break becomes
        a space, blank lines become line breaks, and lines indented further
        than the first keep their line breaks."""
        value = lines[0]
        previous_indented = False
        blank_lines = 0
        for line in lines[1:]:
            if line == '':
                blank_lines += 1
                continue
            indented = line.startswith(' ')
            if blank_lines:
                separator = '\n' * (blank_lines + (1 if previous_indented or indented else 0))
            else:
                separator = '\n' if previous_indented or indented else ' '
            value += separator + line
            previous_indented = indented
            blank_lines = 0
        return value


def parse_document(text, name, allow_comments=False):
    """Read a YAML document whose root is a block mapping and return its node
    tree. name is used in error messages. Comments are refused unless
    allow_comments is set, because emit_document() would not write them back.
    """
    return _Reader(text, name, allow_comments).read_document()


# --------------------------------------------------------------------------
# Writer
# --------------------------------------------------------------------------
#
# Line lengths are counted in UTF-16 code units, as the JavaScript package
# counts them; text is converted to that form while it is laid out.

def _to_utf16_units(text):
    if all(ord(char) <= 0xFFFF for char in text):
        return text
    data = text.encode('utf-16-le')
    return ''.join(chr(data[i] | data[i + 1] << 8) for i in range(0, len(data), 2))


def _from_utf16_units(text):
    return text.encode('utf-16-le', 'surrogatepass').decode('utf-16-le')


def _char(text, index):
    """text[index], or '' past either end (JavaScript's undefined)."""
    return text[index] if 0 <= index < len(text) else ''


FOLD_FLOW, FOLD_BLOCK, FOLD_QUOTED = 'flow', 'block', 'quoted'


def _fold_lines(text, indent, mode, indent_at_start=None, on_overflow=None):
    """Break text into lines of at most LINE_WIDTH columns by replacing single
    spaces with a line break plus indent. indent_at_start is the column the
    first line starts at. In FOLD_QUOTED mode a line without a usable space is
    broken with a backslash; otherwise it is left long and on_overflow is
    called."""
    end_step = max(1 + MIN_CONTENT_WIDTH, 1 + LINE_WIDTH - len(indent))
    if len(text) <= end_step:
        return text
    folds = []
    escaped_folds = set()
    end = LINE_WIDTH - len(indent)
    if indent_at_start is not None:
        if indent_at_start > LINE_WIDTH - max(2, MIN_CONTENT_WIDTH):
            folds.append(0)
        else:
            end = LINE_WIDTH - indent_at_start
    split = None
    previous = ''
    overflow = False
    index = -1
    escape_start = -1
    escape_end = -1
    if mode == FOLD_BLOCK:
        index = _skip_more_indented_lines(text, index, len(indent))
        if index != -1:
            end = index + end_step
    while True:
        index += 1
        char = _char(text, index)
        if not char:
            break
        if mode == FOLD_QUOTED and char == '\\':
            escape_start = index
            index += {'x': 3, 'u': 5, 'U': 9}.get(_char(text, index + 1), 1)
            escape_end = index
        if char == '\n':
            if mode == FOLD_BLOCK:
                index = _skip_more_indented_lines(text, index, len(indent))
            end = index + len(indent) + end_step
            split = None
        else:
            if char == ' ' and previous and previous not in ' \n\t':
                # A space between two non-spaces can become a line break.
                following = _char(text, index + 1)
                if following and following not in ' \n\t':
                    split = index
            if index >= end:
                if split:
                    folds.append(split)
                    end = split + end_step
                    split = None
                elif mode == FOLD_QUOTED:
                    # Whitespace collected at the end may stretch past the width.
                    while previous in (' ', '\t'):
                        previous = char
                        index += 1
                        char = _char(text, index)
                        overflow = True
                    # Break with a backslash, but never inside an escape.
                    fold_at = index - 2 if index > escape_end + 1 else escape_start - 1
                    if fold_at in escaped_folds:
                        return text
                    folds.append(fold_at)
                    escaped_folds.add(fold_at)
                    end = fold_at + end_step
                    split = None
                else:
                    overflow = True
        previous = char
    if overflow and on_overflow:
        on_overflow()
    if not folds:
        return text
    result = text[:folds[0]]
    for number, fold in enumerate(folds):
        stop = folds[number + 1] if number + 1 < len(folds) else len(text)
        if fold == 0:
            result = f'\n{indent}{text[:stop]}'
        else:
            if mode == FOLD_QUOTED and fold in escaped_folds:
                result += f'{text[fold]}\\'
            result += f'\n{indent}{text[fold + 1:stop]}'
    return result


def _skip_more_indented_lines(text, index, indent):
    """In a folded block scalar, lines indented beyond the block are never
    folded. index + 1 is the start of a line; return the index of the last
    line break of the run of such lines that starts there."""
    end = index
    start = index + 1
    char = _char(text, start)
    while char in (' ', '\t'):
        if index < start + indent:
            index += 1
            char = _char(text, index)
        else:
            while True:
                index += 1
                char = _char(text, index)
                if not char or char == '\n':
                    break
            end = index
            start = index + 1
            char = _char(text, start)
    return end


_DOCUMENT_MARKER = re.compile(r'^(%|---|\.\.\.)', re.M)
_NEEDS_QUOTES = re.compile(
    r'''\A[\n\t ,\[\]{}#&*!|>'"%@`]|\A[?-]\Z|\A[?-][ \t]|[\n:][ \t]|[ \t]\n|[\n\t ]#|[\n\t :]\Z''')
_FLOW_INDICATOR = re.compile(r'[\[\]{},]')
_CONTROL_CHARACTER = re.compile(r'[\x00-\x08\x0b-\x1f\x7f-\x9f]')
# A plain string spelled like one of these would be read back as a null,
# boolean or number, so it must be quoted.
_READS_AS_NON_STRING = re.compile(
    r'(?:~|[Nn]ull|NULL)?|[Tt]rue|TRUE|[Ff]alse|FALSE|0o[0-7]+|[-+]?[0-9]+|0x[0-9a-fA-F]+|'
    r'[-+]?\.(?:inf|Inf|INF)|\.nan|\.NaN|\.NAN|'
    r'[-+]?(?:\.[0-9]+|[0-9]+(?:\.[0-9]*)?)[eE][-+]?[0-9]+|[-+]?(?:\.[0-9]+|[0-9]+\.[0-9]*)')
_DOUBLE_QUOTED_MIN_MULTI_LINE_LENGTH = 40
_SHORT_ESCAPES = {'0000': '\\0', '0007': '\\a', '000b': '\\v', '001b': '\\e'}


class _Layout:
    """Where a scalar is being written: the indent for its continuation
    lines, whether it sits inside a flow collection, whether it is a mapping
    key (keys stay on one line), and the column its first line starts at."""

    def __init__(self, indent, in_flow, implicit_key=False, indent_at_start=None):
        self.indent = indent
        self.in_flow = in_flow
        self.implicit_key = implicit_key
        self.indent_at_start = indent_at_start


def _double_quoted(value, layout):
    json = json_quote(value)
    result = ''
    start = 0
    index = 0
    char = _char(json, 0)
    while char:
        if char == ' ' and json[index + 1:index + 3] == '\\n':
            # A space before a line break is escaped so folding keeps it.
            result += json[start:index] + '\\ '
            index += 1
            start = index
            char = '\\'
        if char == '\\':
            following = _char(json, index + 1)
            if following == 'u':
                result += json[start:index]
                code = json[index + 2:index + 6]
                if code in _SHORT_ESCAPES:
                    result += _SHORT_ESCAPES[code]
                elif code.startswith('00'):
                    result += '\\x' + code[2:]
                else:
                    result += json[index:index + 6]
                index += 5
                start = index + 1
            elif following == 'n':
                if (layout.implicit_key or _char(json, index + 2) == '"'
                        or len(json) < _DOUBLE_QUOTED_MIN_MULTI_LINE_LENGTH):
                    index += 1
                else:
                    # Write the line break as a real one; folding eats the first.
                    result += json[start:index] + '\n\n'
                    while (json[index + 2:index + 4] == '\\n'
                           and _char(json, index + 4) != '"'):
                        result += '\n'
                        index += 2
                    result += layout.indent
                    if _char(json, index + 2) == ' ':
                        result += '\\'
                    index += 1
                    start = index + 1
            else:
                index += 1
        index += 1
        char = _char(json, index)
    result = result + json[start:] if start else json
    if layout.implicit_key:
        return result
    return _fold_lines(result, layout.indent, FOLD_QUOTED, layout.indent_at_start)


def _single_quoted(value, layout):
    if (layout.implicit_key and '\n' in value) or re.search(r'[ \t]\n|\n[ \t]', value):
        return _double_quoted(value, layout)
    result = "'" + re.sub(r'\n+', lambda m: m.group() + '\n' + layout.indent,
                          value.replace("'", "''")) + "'"
    if layout.implicit_key:
        return result
    return _fold_lines(result, layout.indent, FOLD_FLOW, layout.indent_at_start)


def _quoted(value, layout):
    """Quote a string that had no quotes of its own: double quotes unless the
    text contains double quotes and no single quote."""
    if '"' in value and "'" not in value:
        return _single_quoted(value, layout)
    return _double_quoted(value, layout)


def _line_length_over_limit(text, limit):
    if len(text) <= limit:
        return False
    start = 0
    for index, char in enumerate(text):
        if char == '\n':
            if index - start > limit:
                return True
            start = index + 1
            if len(text) - start <= limit:
                return False
    return True


def _block_scalar(value, style, layout):
    """Write a multi-line string as a literal (|) or folded (>) block."""
    if re.search(r'\n[\t ]+\Z', value):
        return _quoted(value, layout)
    indent = layout.indent
    if style == FOLDED_BLOCK:
        literal = False
    elif style == LITERAL_BLOCK:
        literal = True
    else:
        literal = not _line_length_over_limit(value, LINE_WIDTH - len(indent))
    if value == '' or value[0] in ' \t\n':
        raise YamlSubsetError('cannot write a block scalar that is empty or starts with whitespace')
    # The chomping indicator follows from the whitespace at the end.
    end_start = len(value.rstrip('\n\t '))
    end = value[end_start:]
    newline = end.find('\n')
    if newline == -1:
        chomp = '-'
    elif value == end or newline != len(end) - 1:
        raise YamlSubsetError('cannot write a block scalar that ends with blank lines')
    else:
        chomp = ''
    if end:
        value = value[:-len(end)]
        if end.endswith('\n'):
            end = end[:-1]
    if not literal:
        folded = re.sub(r'\n+', lambda m: '\n' + m.group(), value)
        # Lines indented further than the block are not folded.
        folded = re.sub(r'(?:\A|\n)([\t ].*)(?:([\n\t ]*)\n(?![\n\t ]))?',
                        lambda m: m.group(1) + (m.group(2) or ''), folded)
        folded = re.sub(r'\n+', lambda m: m.group() + indent, folded)
        overflowed = []
        on_overflow = None if style == FOLDED_BLOCK else lambda: overflowed.append(True)
        body = _fold_lines(folded + end, indent, FOLD_BLOCK, len(layout.indent), on_overflow)
        if not overflowed:
            return f'>{chomp}\n{indent}{body}'
    value = re.sub(r'\n+', lambda m: m.group() + indent, value)
    return f'|{chomp}\n{indent}{value}{end}'


def _plain(value, style, layout):
    """Write a string without quotes where YAML allows it, else quote it."""
    if (layout.implicit_key and '\n' in value) or (layout.in_flow and _FLOW_INDICATOR.search(value)):
        return _quoted(value, layout)
    if _NEEDS_QUOTES.search(value):
        if layout.implicit_key or layout.in_flow or '\n' not in value:
            return _quoted(value, layout)
        return _block_scalar(value, style, layout)
    if not layout.implicit_key and not layout.in_flow and style != PLAIN and '\n' in value:
        return _block_scalar(value, style, layout)
    if _DOCUMENT_MARKER.search(value) and layout.implicit_key and layout.indent == INDENT_STEP:
        return _quoted(value, layout)
    text = re.sub(r'\n+', lambda m: m.group() + '\n' + layout.indent, value)
    if _READS_AS_NON_STRING.fullmatch(text):
        return _quoted(value, layout)
    if layout.implicit_key:
        return text
    return _fold_lines(text, layout.indent, FOLD_FLOW, layout.indent_at_start)


def _string(scalar, layout):
    value = _to_utf16_units(scalar.value)
    if layout.indent == '':
        raise YamlSubsetError('internal error: scalar outside a collection')
    style = scalar.style
    if style != DOUBLE_QUOTED and _CONTROL_CHARACTER.search(value):
        style = DOUBLE_QUOTED
    if style in (LITERAL_BLOCK, FOLDED_BLOCK):
        if layout.implicit_key or layout.in_flow:
            return _quoted(value, layout)
        return _block_scalar(value, style, layout)
    if style == DOUBLE_QUOTED:
        return _double_quoted(value, layout)
    if style == SINGLE_QUOTED:
        return _single_quoted(value, layout)
    return _plain(value, style, layout)


def _number(scalar):
    value = scalar.value
    text = js_number_text(value)
    # A decimal written with trailing zeros (1.0, 2.50) keeps them.
    digits = scalar.min_fraction_digits
    if digits and 'e' not in text:
        if '.' not in text:
            text += '.'
        text += '0' * (digits - (len(text) - text.index('.') - 1))
    return text


def _scalar(scalar, layout):
    value = scalar.value
    if value is None:
        return scalar.source if scalar.source is not None else 'null'
    if isinstance(value, bool):
        if scalar.source and (scalar.source[0] in 'tT') == value:
            return scalar.source
        return 'true' if value else 'false'
    if isinstance(value, (int, float)):
        return _number(scalar)
    return _string(scalar, layout)


def _node(node, indent, in_flow, indent_at_start=None):
    if isinstance(node, Scalar):
        return _scalar(node, _Layout(indent, in_flow, False, indent_at_start))
    is_mapping = isinstance(node, Mapping)
    children = node.entries if is_mapping else node.items
    brackets = '{}' if is_mapping else '[]'
    item_indent = indent if is_mapping else indent + '  '
    # A flow mapping made only of bare keys ({ a, b }) is written like a set.
    bare_keys = is_mapping and all(entry.value is None for entry in children)

    def child_text(child, child_indent, child_in_flow):
        if is_mapping:
            return _entry(child, child_indent, child_in_flow, bare_keys)
        return _node(child, child_indent, child_in_flow)

    if in_flow or node.flow:
        item_indent += INDENT_STEP
        lines = [child_text(child, item_indent, True) for child in children]
        if not lines:
            return brackets
        lines = [line + ',' for line in lines[:-1]] + lines[-1:]
        one_line_length = sum(len(line) + 2 for line in lines) + 2
        if any('\n' in line for line in lines) or one_line_length > LINE_WIDTH:
            body = ''.join(f'\n{INDENT_STEP}{indent}{line}' for line in lines)
            return f'{brackets[0]}{body}\n{indent}{brackets[1]}'
        return f'{brackets[0]} {" ".join(lines)} {brackets[1]}'

    lines = []
    for child in children:
        if child.space_before:
            lines.append('')
        lines.append(('' if is_mapping else '- ') + child_text(child, item_indent, False))
    if not lines:
        return brackets
    return lines[0] + ''.join(f'\n{indent}{line}' if line else '\n' for line in lines[1:])


def _entry(entry, indent, in_flow, bare_keys):
    value_indent = indent + INDENT_STEP
    text = _scalar(entry.key, _Layout(value_indent, in_flow, implicit_key=not bare_keys))
    if entry.value is None:
        if not in_flow:
            raise YamlSubsetError('internal error: key without a value outside a flow mapping')
        return text
    if not in_flow and len(text) > 1024:
        raise YamlSubsetError('mapping key is longer than 1024 characters')
    text += ':'
    value = entry.value
    is_scalar = isinstance(value, Scalar)
    value_text = _node(value, value_indent, in_flow, len(text) + 1 if is_scalar else None)
    separator = ' '
    if value.space_before:
        separator = f'\n\n{value_indent}'
    elif not is_scalar:
        children = value.entries if isinstance(value, Mapping) else value.items
        # An empty collection is written as {} or [] on the key's line.
        flow = in_flow or value.flow or not children
        if '\n' in value_text or not flow:
            separator = f'\n{value_indent}'
    elif value_text == '' or value_text[0] == '\n':
        separator = ''
    return text + separator + value_text


def emit_document(root):
    """Write a node tree whose root is a mapping as YAML text."""
    if not isinstance(root, Mapping) or root.flow:
        raise YamlSubsetError('the document must be a block mapping')
    return _from_utf16_units(_node(root, '', False) + '\n')
