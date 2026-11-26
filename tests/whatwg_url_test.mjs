#!/usr/bin/env node
// Replays tests/corpus/whatwg_url_corpus.json — frozen from the
// production TypeScript runtime (node:22-alpine, Ada 2.9.2) by
// scripts/generate_whatwg_url_corpus.mjs — through src/compat/whatwg_url.cpp. Never
// consults the local Node.js URL implementation.
// Usage: node tests/whatwg_url_test.mjs build/whatwg_url_test
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const fixturePath = path.join(root, 'tests/corpus/whatwg_url_corpus.json');
const bytes = readFileSync(fixturePath);
const pinned = readFileSync(`${fixturePath}.sha256`, 'utf8').trim();
if (createHash('sha256').update(bytes).digest('hex') !== pinned) {
  console.error('whatwg_url_corpus.json does not match its pinned hash'); process.exit(1);
}
const fixture = JSON.parse(bytes.toString());
const hex = (text) => (text === '' ? '-' : Buffer.from(text, 'utf8').toString('hex'));
const unhex = (field) => (field === '-' ? '' : Buffer.from(field, 'hex').toString('utf8'));

const lines = [
  ...fixture.cases.map((item) => `P ${hex(item.input)} ${item.base === undefined ? '-' : hex(item.base)}`),
  ...fixture.canonical.map((item) => `C ${hex(item.input)}`),
  ...fixture.exhaustive.map((item) => (item.before === undefined
    ? `X ${hex(item.prefix)} ${hex(item.suffix)}`
    : `Y ${hex(item.prefix)} ${hex(item.suffix)} ${hex(item.before)} ${hex(item.after)}`)),
];
const run = spawnSync(process.argv[2], { input: `${lines.join('\n')}\n`, maxBuffer: 1 << 28 });
if (run.status !== 0) { console.error(`binary exited ${run.status}`); process.exit(1); }
const output = run.stdout.toString().trimEnd().split('\n');
const KEYS = ['href', 'protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname',
  'search', 'hash'];
// Native bound (src/compat/whatwg_url.h kMaxInputBytes): longer inputs are refused.
const MAX_INPUT_BYTES = 8192;
const short = (text) => JSON.stringify(text.length > 120 ? `${text.slice(0, 117)}...` : text);
let index = 0;
let failures = 0;
let bounded = 0;
const fail = (message) => { if (failures++ < 40) console.error(message); };
for (const item of fixture.cases) {
  const actual = output[index++];
  const label = `${short(item.input)}${item.base === undefined ? '' : ` against ${short(item.base)}`}`;
  if (Buffer.byteLength(item.input, 'utf8') > MAX_INPUT_BYTES) {
    if (actual !== 'null') fail(`${label}: an input over ${MAX_INPUT_BYTES} bytes must be refused`);
    bounded++;
    continue;
  }
  if (item.result === null) {
    if (actual !== 'null') fail(`${label}: expected a throw, got ${actual.split('\t').map(unhex)[0]}`);
    continue;
  }
  if (actual === 'null' || actual === 'basefail') { fail(`${label}: expected ${item.result.href}, got ${actual}`); continue; }
  const fields = actual.split('\t').map(unhex);
  KEYS.forEach((key, i) => {
    if (fields[i] !== item.result[key]) {
      fail(`${label}: ${key} expected ${JSON.stringify(item.result[key])}, got ${JSON.stringify(fields[i])}`);
    }
  });
}
for (const item of fixture.canonical) {
  const actual = output[index++];
  const value = actual === 'null' ? null : unhex(actual);
  if (value !== item.result) {
    fail(`canonicalUrl(${JSON.stringify(item.input)}): expected ${JSON.stringify(item.result)}, got ${JSON.stringify(value)}`);
  }
}
let probes = 0;
for (const item of fixture.exhaustive) {
  const [count, total, ...blocks] = output[index++].split(' ');
  probes += Number(count);
  if (Number(count) !== item.count) fail(`exhaustive ${JSON.stringify(item.prefix)}…: ${count} probes, expected ${item.count}`);
  if (total !== item.sha256) {
    const bad = blocks.flatMap((hash, block) => (hash === item.blocks[block] ? [] : [block]));
    fail(`exhaustive ${JSON.stringify(item.prefix)}<cp>${JSON.stringify(item.suffix)}: hash differs in blocks ${bad.join(',')}`);
  }
}
if (failures) { console.error(`${failures} mismatches`); process.exit(1); }
console.log(`whatwg url: ${fixture.cases.length} cases, ${fixture.canonical.length} canonical URLs, ` +
  `${probes} exhaustive code-point probes match ${fixture.source.image.split('@')[0]} (Ada ${fixture.source.ada}); ` +
  `${bounded} over-bound input refused by design`);
