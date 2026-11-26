#!/usr/bin/env node
// Differential test of src/compat/js_json.cpp: JSON.stringify(JSON.parse(text)),
// protobuf-es 2.x toBinary(ValueSchema, fromJson(ValueSchema, value)) and the
// top-level array splitter, over hand-written edge cases and a seeded random
// corpus. Usage: node tests/js_json_test.mjs build/js_json_test
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const binary = process.argv[2];
if (!binary) { console.error('usage: js_json_test.mjs <binary>'); process.exit(2); }
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const protobuf = await import(pathToFileURL(
  path.join(root, 'node_modules/@bufbuild/protobuf/dist/esm/index.js')).href);
const wkt = await import(pathToFileURL(
  path.join(root, 'node_modules/@bufbuild/protobuf/dist/esm/wkt/index.js')).href);

const fixed = [
  'null', 'true', 'false', '0', '-0', '1e21', '1e-7', '123456789012345678901234', '0.1',
  '5e-324', '1.7976931348623157e308', '-1.5E+3', '"plain"', '"esc \\" \\\\ \\/ \\b \\f \\n \\r \\t"',
  '"\\u0001\\u001f\\u007f\\u2028\\u00e9\\ud83d\\ude00"', '"é😀"', '[]', '{}', '[1,[2,[3,{}]]]',
  '{"b":1,"a":2,"b":3}', '{"2":"x","1":"y","a":0,"01":1,"4294967294":2,"4294967295":3,"-1":4}',
  '{"\\u0031":1,"1":2}', '{"":1,"":null}', ' [ 1 , 2 ] ', '[{"a":[]},{"b":{}}]',
  '{"__proto__":1}', '{"x":{"y":{"z":[null,true,false,"s",1.25]}}}',
  '[ "a,b", "]", "[", "\\"" ]', '{"k":"v"}',
];
let seed = 11;
const random = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
const keys = ['a', 'b', '0', '1', '10', '2', 'id', 'é', '😀', '', 'a"b', '01', '9'];
function value(depth) {
  const pick = random() % (depth > 4 ? 5 : 8);
  if (pick === 0) return null;
  if (pick === 1) return random() % 2 === 0;
  if (pick === 2) return [0, -1, 3.14159, 1e21, 1e-7, 2 ** 53 + 1, -0.000001, 42][random() % 8];
  if (pick === 3 || pick === 4) return ['', 'x', 'é', '\u0000', '\n', '"', '😀', 'long'.repeat(20)][random() % 8];
  if (pick === 5) return Array.from({ length: random() % 4 }, () => value(depth + 1));
  const entries = Array.from({ length: random() % 5 }, () => [keys[random() % keys.length], value(depth + 1)]);
  return entries;  // serialized with possible duplicates below
}
function text(v) {
  if (Array.isArray(v) && v.every((entry) => Array.isArray(entry) && entry.length === 2 && typeof entry[0] === 'string')) {
    return `{${v.map(([k, x]) => `${JSON.stringify(k)}:${text(x)}`).join(',')}}`;
  }
  if (Array.isArray(v)) return `[${v.map(text).join(',')}]`;
  return JSON.stringify(v);
}
const documents = [...fixed];
for (let i = 0; i < 3000; i++) documents.push(text(value(0)));
const input = documents.map((doc) => Buffer.from(doc, 'utf8').toString('hex')).join('\n') + '\n';
const run = spawnSync(binary, { input, maxBuffer: 1 << 28 });
if (run.status !== 0) { console.error(`binary exited ${run.status}`); process.exit(1); }
const lines = run.stdout.toString().trimEnd().split('\n');
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
let failures = 0, refused = 0;
documents.forEach((doc, index) => {
  const parsed = orderedParse(doc);
  const line = lines[index];
  if (line === '!') { refused++; console.error(`refused: ${doc}`); failures++; return; }
  const [jsonHex, protoHex, count] = line.split(' ');
  const expectedJson = Buffer.from(orderedJson(parsed), 'utf8').toString('hex');
  const expectedProto = orderedProto(parsed).toString('hex');
  const expectedCount = String(Array.isArray(parsed) ? parsed.length : -1);
  for (const [name, actual, expected] of [['stringify', jsonHex, expectedJson],
    ['protobuf', protoHex, expectedProto], ['elements', count, expectedCount]]) {
    if (actual !== expected && failures++ < 10) {
      console.error(`${name} ${doc}\n  expected ${expected}\n  actual   ${actual}`);
    }
  }
});
if (failures) { console.error(`${failures} mismatches (${refused} refused)`); process.exit(1); }
console.log(`js json: ${documents.length} documents match source-order JSON and protobuf scalar/wire reference ${'2.x'}`);
