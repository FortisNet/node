#!/usr/bin/env node
// Maintenance tool, NOT part of the build or of `make test`.
//
// Records the reference answers for src/compat/js_json.cpp into
// tests/corpus/js_json_expectations.json (and its .sha256 sidecar), which
// tests/js_json_test.py replays without any JavaScript engine. It needs
// Node.js and node_modules/@bufbuild/protobuf (protobuf-es 2.x):
//
//     node tests/reference/record_js_json.mjs
//
// The documents are the hand-written edge cases of the earlier live test
// (tests/js_json_test.mjs) and 3000 documents from a seeded generator (see
// below; it replaces the earlier test's generator, which produced almost only
// `null`). The reference (orderedParse / orderedJson / orderedProto) is that
// test's, unchanged. The generator's output is written out as data, so the
// Python test never has to reproduce JavaScript's arithmetic.
//
// For each document three answers are recorded:
//   json     - JSON.stringify of every scalar, with object members kept in
//              source order (a repeated key keeps its first position and its
//              last value). JavaScript's own object enumeration (integer-like
//              keys first) is deliberately NOT the contract.
//   proto    - the google.protobuf.Value bytes (hex): protobuf-es
//              toBinary(ValueSchema, fromJson(ValueSchema, scalar)) for
//              scalars, wrapped by hand for objects and lists in that same
//              source order.
//   elements - the number of top-level array elements, or -1 when the
//              document is not an array.
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const target = path.join(root, 'tests/corpus/js_json_expectations.json');
const protobuf = await import(pathToFileURL(
  path.join(root, 'node_modules/@bufbuild/protobuf/dist/esm/index.js')).href);
const wkt = await import(pathToFileURL(
  path.join(root, 'node_modules/@bufbuild/protobuf/dist/esm/wkt/index.js')).href);
const protobufVersion = JSON.parse(readFileSync(
  path.join(root, 'node_modules/@bufbuild/protobuf/package.json'), 'utf8')).version;

// --- hand-written documents (unchanged from tests/js_json_test.mjs) ---------
const fixed = [
  'null', 'true', 'false', '0', '-0', '1e21', '1e-7', '123456789012345678901234', '0.1',
  '5e-324', '1.7976931348623157e308', '-1.5E+3', '"plain"', '"esc \\" \\\\ \\/ \\b \\f \\n \\r \\t"',
  '"\\u0001\\u001f\\u007f\\u2028\\u00e9\\ud83d\\ude00"', '"é😀"', '[]', '{}', '[1,[2,[3,{}]]]',
  '{"b":1,"a":2,"b":3}', '{"2":"x","1":"y","a":0,"01":1,"4294967294":2,"4294967295":3,"-1":4}',
  '{"\\u0031":1,"1":2}', '{"":1,"":null}', ' [ 1 , 2 ] ', '[{"a":[]},{"b":{}}]',
  '{"__proto__":1}', '{"x":{"y":{"z":[null,true,false,"s",1.25]}}}',
  '[ "a,b", "]", "[", "\\"" ]', '{"k":"v"}',
];
// Seeded generator. The earlier test computed
// `seed = (seed * 1103515245 + 12345) & 0x7fffffff` in double arithmetic: the
// product exceeds 2^53, the low bits are lost, and 2992 of its 3000 documents
// were `null`. This one is exact in 32 bits (Math.imul) and takes its choices
// from the high bits. The documents are written as JSON text directly (not
// through JSON.stringify), so that number spellings, escapes, white space and
// repeated keys reach the parser as a provider could send them.
// Everything stays inside the documented bounds of the code under test
// (src/compat/js_json.h, src/codec/bounded_json.h): nesting at most 16,
// number literals shorter than 64 bytes and finite, valid UTF-8.
let seed = 11;
const random = () => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed >>> 16; };
const pick = (n) => random() % n;
const oneOf = (list) => list[pick(list.length)];
const MAX_DEPTH = 16;  // kBoundedJsonMaxDepth

const NUMBERS = [
  '0', '-0', '-0.0', '0.0', '0e0', '0E-0', '-0e5', '1', '-1', '42', '100', '1e2', '1E+2', '1.0', '1.50',
  '-1.5E+3', '0.1', '0.2', '0.30000000000000004', '4.35', '1.005', '3.14159', '-0.000001', '0.000001',
  '1e-6', '1e-7', '1E-7', '1.0E-7', '0.000001234', '123e-20', '1e21', '1E21', '1e+21', '999999999999999900000',
  '999999999999999999999', '123456789012345678901234', '9007199254740991', '9007199254740992',
  '9007199254740993', '-9007199254740993', '18446744073709551615', '4294967295', '4294967296', '2147483648',
  '-2147483649', '1234567890.123456789', '5e-324', '4.9e-324', '2.5e-324', '3e-324', '-1e-324', '1e-400',
  '2.2250738585072014e-308', '2.225073858507201e-308', '1e300', '1e-300', '1.7976931348623157e308',
  '1.7976931348623157E+308', '1.7976931348623158e308', '-1.7976931348623157e308', '0.1e1', '10e-1',
  '123456789012345678901234567890123456789012345678901234567890123',  // 63 bytes: the longest accepted
  '0.00000000000000000000000000000000000000000000000000000000001',
];
const digits = (count) => { let out = ''; for (let i = 0; i < count; i++) out += String(pick(10)); return out; };
function numberLiteral() {
  const kind = pick(10);
  if (kind < 4) return oneOf(NUMBERS);
  let out = pick(4) === 0 ? '-' : '';
  const whole = pick(3) === 0 ? 0 : 1 + pick(kind === 9 ? 24 : 6);
  out += whole === 0 ? '0' : String(1 + pick(9)) + digits(whole - 1);
  if (pick(2) === 0) out += '.' + digits(1 + pick(kind === 8 ? 17 : 4));
  if (pick(3) === 0) {
    // |exponent| <= 280 with at most 24 integer digits keeps the value finite.
    const exponent = kind === 7 ? 250 + pick(31) : pick(30);
    out += oneOf(['e', 'E']) + oneOf(['', '+', '-', '-']) + String(exponent);
  }
  return out;
}
// Pieces of a string literal, already in JSON source form.
const PIECES = [
  '', 'x', 'abc', 'Hello', ' ', 'é', 'ß', 'İ', 'Σς', '中文', '😀', '\u{10400}', '\u{10ffff}', 'e\u0301',
  '\u007f', '\u0080', '\u00a0', '\u2028', '\u2029', '\ufeff', '\ufffd', '\uffff',
  '\\"', '\\\\', '\\/', '/', '\\b', '\\f', '\\n', '\\r', '\\t', '\\u0000', '\\u0001', '\\u001f', '\\u001F',
  '\\u007f', '\\u0041', '\\u00e9', '\\u00E9', '\\u2028', '\\u2029', '\\ufeff', '\\uffff', '\\ud83d\\ude00',
  '\\uD83D\\uDE00', '\\udbff\\udfff', '<', '>', '&', '\'', '\\u0022', '\\u005c', '[', ']', '{', '}', ',', ':',
];
// A surrogate escape without its partner. JSON.parse yields a lone surrogate;
// the server (and this reference) replace it with U+FFFD.
const LONE = ['\\ud800', '\\udbff', '\\udc00', '\\udfff', '\\ud83d', '\\ude00'];
function stringLiteral() {
  let out = '"';
  const count = pick(7);
  for (let i = 0; i < count; i++) {
    const kind = pick(40);
    out += kind === 0 ? oneOf(LONE) : kind === 1 ? 'long'.repeat(20) : oneOf(PIECES);
  }
  return out + '"';
}
// Keys in JSON source form. Several spell the same key differently ("a" and
// "a", "1" and "1"), integer-like keys mix with names (JavaScript
// objects would reorder those; the contract is source order), and the small
// pool makes repeated keys common.
const KEYS = ['"a"', '"b"', '"0"', '"1"', '"10"', '"2"', '"id"', '"é"', '"😀"', '""', '"a\\"b"', '"01"', '"9"',
  '"\\u0061"', '"\\u0031"', '"A"', '"-1"', '"4294967294"', '"4294967295"', '"1.5"', '"__proto__"',
  '"constructor"', '"toString"', '"length"', '"a b"', '" "', '"\\u00e9"', '"\\ud83d\\ude00"', '"\u2028"',
  '"name"', '"price"', '"z"'];
const space = () => (pick(12) === 0 ? oneOf([' ', '  ', '\n', '\t', '\r\n', ' \n\t']) : '');
function value(depth, limit) {
  // Scalars only below `limit`; the root of a container document (depth 0)
  // is always a container.
  const kind = depth >= limit ? pick(7) : depth === 0 ? 7 + pick(5) : pick(12);
  if (kind === 0) return 'null';
  if (kind === 1) return oneOf(['true', 'false']);
  if (kind <= 3) return numberLiteral();
  if (kind <= 6) return stringLiteral();
  if (kind <= 8) {
    const items = [];
    for (let count = pick(6) || (depth === 0 ? 1 : 0); count > 0; count--) items.push(space() + value(depth + 1, limit) + space());
    return `[${items.length ? items.join(',') : space()}]`;
  }
  const members = [];
  for (let count = pick(7) || (depth === 0 ? 1 : 0); count > 0; count--) {
    const key = pick(10) === 0 ? stringLiteral() : oneOf(KEYS);
    members.push(`${space()}${key}${space()}:${space()}${value(depth + 1, limit)}${space()}`);
  }
  return `{${members.length ? members.join(',') : space()}}`;
}
// A chain of containers down to exactly the deepest accepted nesting.
function deepest() {
  let open = '', close = '';
  for (let level = 0; level < MAX_DEPTH; level++) {
    if (pick(2) === 0) { open += '['; close = ']' + close; }
    else { open += `{${oneOf(KEYS)}:`; close = '}' + close; }
  }
  // The innermost container holds one scalar, or nothing when it is an array.
  if (open.endsWith(':') || pick(2) === 0) open += value(MAX_DEPTH, 0);
  return open + close;
}
const SEEDED = 3000;
const documents = [...fixed];
for (let i = 0; i < SEEDED; i++) {
  const shape = pick(40);
  let doc;
  if (shape === 0) doc = deepest();
  else if (shape < 6) doc = value(MAX_DEPTH, 0);                 // a bare scalar
  else doc = value(0, shape < 30 ? 3 : 5);                      // containers, usually shallow
  documents.push(space() + doc + space());
}
const input = documents.map((doc) => Buffer.from(doc, 'utf8').toString('hex')).join('\n') + '\n';
// --- reference, copied unchanged from tests/js_json_test.mjs ----------------
// Maps preserve source/insertion order, including integer-string keys. Node's
// object enumeration no longer defines this server's object contract. Keep
// Node/protobuf-es as the independent scalar/number/presence oracle.
function orderedParse(doc) {
  const tokens = doc.match(/"(?:[^"\\]|\\.)*"|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null|[{}\[\]:,]/g);
  let at = 0;
  const take = () => tokens[at++];
  function value() {
    const token = take();
    if (token === '{') {
      const map = new Map();
      if (tokens[at] === '}') { take(); return map; }
      do {
        const key = Buffer.from(JSON.parse(take()), 'utf8').toString('utf8');
        if (take() !== ':') throw new Error('expected colon');
        map.set(key, value());
      } while (tokens[at] === ',' && take());
      if (take() !== '}') throw new Error('expected object end');
      return map;
    }
    if (token === '[') {
      const array = [];
      if (tokens[at] === ']') { take(); return array; }
      do { array.push(value()); } while (tokens[at] === ',' && take());
      if (take() !== ']') throw new Error('expected array end');
      return array;
    }
    const scalar = JSON.parse(token);
    return typeof scalar === 'string' ? Buffer.from(scalar, 'utf8').toString('utf8') : scalar;
  }
  const result = value();
  if (at !== tokens.length) throw new Error('unconsumed input');
  return result;
}
function orderedJson(value) {
  if (value instanceof Map) return `{${[...value].map(([k,v]) => `${JSON.stringify(k)}:${orderedJson(v)}`).join(',')}}`;
  if (Array.isArray(value)) return `[${value.map(orderedJson).join(',')}]`;
  return JSON.stringify(value);
}
function varint(number) {
  const bytes = [];
  while (number >= 128) { bytes.push((number % 128) | 128); number = Math.floor(number / 128); }
  bytes.push(number);
  return Buffer.from(bytes);
}
function message(field, bytes) { return Buffer.concat([varint(field * 8 + 2), varint(bytes.length), bytes]); }
function orderedProto(value) {
  if (value instanceof Map) return message(5, Buffer.concat([...value].map(([key, child]) =>
    message(1, Buffer.concat([message(1, Buffer.from(key)), message(2, orderedProto(child))])))));
  if (Array.isArray(value)) return message(6, Buffer.concat(value.map(child => message(1, orderedProto(child)))));
  return Buffer.from(protobuf.toBinary(wkt.ValueSchema, protobuf.fromJson(wkt.ValueSchema, value)));
}
// ---------------------------------------------------------------------------

const records = documents.map((doc) => {
  const parsed = orderedParse(doc);
  const json = orderedJson(parsed);
  // A lone surrogate (JavaScript strings are UTF-16) survives the data file:
  // JSON.stringify below writes it as an escape (\udXXX), Python's json module
  // reads it back as the same lone surrogate, and both sides turn it into
  // U+FFFD (ef bf bd) when making UTF-8, as Buffer.from(text, 'utf8') does.
  return {
    text: doc,
    json,
    proto: orderedProto(parsed).toString('hex'),
    elements: Array.isArray(parsed) ? parsed.length : -1,
  };
});

const header = {
  about: 'Recorded reference answers for src/compat/js_json.cpp, replayed by ' +
    'tests/js_json_test.py. Do not edit by hand.',
  engine: `Node.js ${process.versions.node} (V8 ${process.versions.v8})`,
  node: process.versions.node,
  protobuf: `@bufbuild/protobuf ${protobufVersion}`,
  protobufLine: '2.x',
  recorder: 'tests/reference/record_js_json.mjs',
  regenerate: 'node tests/reference/record_js_json.mjs',
  layout: {
    text: 'the JSON document sent to the driver (as the hex of its UTF-8 bytes)',
    json: 'expected output text: JSON.stringify of each scalar, object members in source order',
    proto: 'expected google.protobuf.Value bytes in hex (protobuf-es for scalars, source order for members)',
    elements: 'expected number of top-level array elements; -1 when the document is not an array',
  },
  fixed: fixed.length,
  seeded: documents.length - fixed.length,
  // The bytes to write to the driver's standard input, encoded as the earlier
  // test did (hex of Buffer.from(text, 'utf8'), one document per line).
  stdinSha256: createHash('sha256').update(input).digest('hex'),
};
const body = `{\n  "source": ${JSON.stringify(header, null, 2).replace(/\n/g, '\n  ')},\n` +
  `  "documents": [\n${records.map((record) => `    ${JSON.stringify(record)}`).join(',\n')}\n  ]\n}\n`;
JSON.parse(body);
writeFileSync(target, body);
writeFileSync(`${target}.sha256`, `${createHash('sha256').update(body).digest('hex')}\n`);
console.log(`recorded ${documents.length} documents (${fixed.length} hand-written, ` +
  `${documents.length - fixed.length} seeded) from Node.js ${process.versions.node} and ` +
  `@bufbuild/protobuf ${protobufVersion} into ${path.relative(root, target)}`);
