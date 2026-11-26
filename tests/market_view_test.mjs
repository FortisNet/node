#!/usr/bin/env node
// Replays tests/corpus/market_view_corpus.json (frozen from the earlier
// TypeScript implementation's market view by its tests)
// through src/discovery/market_view.cpp. A non-object row has no native entry: the
// route never builds one (TypeScript reads such rows as unlisted).
// Usage: node tests/market_view_test.mjs build/market_view_test
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const fixturePath = path.join(root, 'tests/corpus/market_view_corpus.json');
const bytes = readFileSync(fixturePath);
const pinned = readFileSync(`${fixturePath}.sha256`, 'utf8').trim();
if (createHash('sha256').update(bytes).digest('hex') !== pinned) {
  console.error('market_view_corpus.json does not match its pinned hash'); process.exit(1);
}
const fixture = JSON.parse(bytes.toString());
const hex = (text) => Buffer.from(text, 'utf8').toString('hex');
const lines = [
  ...fixture.entries.map((item) => `E ${hex(item.id)} ${hex(item.rowText)}`),
  ...fixture.dates.map((item) => `D ${hex(item.text)}`),
  ...fixture.urls.map((item) => `U ${hex(item.text)}`),
];
const run = spawnSync(process.argv[2], { input: lines.join('\n') + '\n', maxBuffer: 1 << 26 });
if (run.status !== 0) { console.error(`binary exited ${run.status}`); process.exit(1); }
const output = run.stdout.toString().trimEnd().split('\n');
// The URL verdicts were frozen under the local Node.js that ran the vitest
// contract (fixture.source.node). Production runs Node.js 22 (Ada 2.9.2),
// which src/compat/whatwg_url.cpp reproduces exactly (tests/whatwg_url_test.mjs).
// Ada 3.x rejects an xn-- label whose punycode decodes to ASCII only, which
// Ada 2.9.2 accepts; such a URL is checked against Ada 2.9.2 instead.
const ADA_2_9_2_ACCEPTS = new Set(['https://xn--abc-/']);
const KNOWN_URL_DIFFERENCES = Number(fixture.source.node.split('.')[0]) > 22
  ? ADA_2_9_2_ACCEPTS : new Set();
let index = 0, failures = 0, known = 0;
const fail = (message) => { if (failures++ < 20) console.error(message); };
for (const item of fixture.entries) {
  const actual = output[index++];
  const isObject = (() => { const v = JSON.parse(item.rowText); return v && typeof v === 'object' && !Array.isArray(v); })();
  if (!isObject) { if (actual !== '-') fail(`${item.id}: expected no entry, got ${actual}`); continue; }
  const text = actual === '!' ? '!' : Buffer.from(actual, 'hex').toString('utf8');
  if (text !== item.entry) fail(`${item.id}:\n  expected ${item.entry}\n  actual   ${text}`);
}
for (const item of fixture.dates) {
  const actual = output[index++] === '1';
  if (actual !== item.valid) fail(`Date.parse(${JSON.stringify(item.text)}): expected ${item.valid}`);
}
for (const item of fixture.urls) {
  const [http, key] = output[index++].split(' ');
  if (KNOWN_URL_DIFFERENCES.has(item.text)) {
    if (http !== '1' || item.http) fail(`url ${item.text}: expected Ada 2.9.2 to accept it over Node ${fixture.source.node}`);
    known++;
    continue;
  }
  if ((http === '1') !== item.http || (item.key ?? '-') !== key) {
    fail(`url ${JSON.stringify(item.text)}: expected ${item.http} ${item.key}, got ${http} ${key}`);
  }
}
if (failures) { console.error(`${failures} mismatches`); process.exit(1); }
console.log(`market view: ${fixture.entries.length} entries, ${fixture.dates.length} dates, ${fixture.urls.length - known} URLs match TypeScript (${known} judged by the production Ada 2.9.2 instead of Node ${fixture.source.node})`);
