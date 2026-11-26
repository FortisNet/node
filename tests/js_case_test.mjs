#!/usr/bin/env node
// Differential test of src/compat/js_case.cpp against this Node.js runtime:
// every scalar value alone (both directions), Final_Sigma contexts, and a
// seeded corpus of mixed strings. Usage: node tests/js_case_test.mjs build/js_case_test
import { spawnSync } from 'node:child_process';

const binary = process.argv[2];
if (!binary) { console.error('usage: js_case_test.mjs <binary>'); process.exit(2); }

const cases = [];
for (let cp = 0; cp <= 0x10ffff; cp++) {
  if (cp >= 0xd800 && cp <= 0xdfff) continue;
  cases.push(String.fromCodePoint(cp));
}
const sigma = ['Σ', 'ΣΣ', 'AΣ', 'AΣB', 'AΣ.', 'AͅΣ', 'AΣͅ', 'AΣͅB', 'A.Σ',
  'A\'Σ', 'ΑΣ ΤΟΣ', 'ΌΣΟΣ', '1Σ', 'AΣ1', 'ὈΔΥΣΣΕΎΣ', 'A­Σ­', 'aΣ‍',
  'İSTANBUL', 'ß straße', 'ŉ', 'ǰ', 'ﬁ', 'ΐ', 'ᾳ', 'ᾼ', 'Ǆǅǆ', '\u{10400}\u{10428}'];
cases.push(...sigma);
let seed = 7;
const random = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
const pool = ['A', 'a', 'Σ', 'σ', 'ς', 'ͅ', '.', '\'', ' ', '­', 'ß', 'İ', 'ı',
  '\u{1e900}', '\u{10400}', 'Ω', 'ǅ', '̇', '0', '😀'];
for (let i = 0; i < 20000; i++) {
  let text = '';
  const length = 1 + random() % 8;
  for (let j = 0; j < length; j++) text += pool[random() % pool.length];
  cases.push(text);
}
const hex = (text) => Buffer.from(text, 'utf8').toString('hex');
const lines = [];
for (const text of cases) { lines.push(`U ${hex(text)}`, `L ${hex(text)}`); }
const run = spawnSync(binary, { input: lines.join('\n') + '\n', maxBuffer: 1 << 28 });
if (run.status !== 0) { console.error(`binary exited ${run.status}`); process.exit(1); }
const output = run.stdout.toString().trimEnd().split('\n');
const version = output.pop();
if (version !== `unicode ${process.versions.unicode}`) {
  console.error(`tables are ${version}, Node.js has Unicode ${process.versions.unicode}`);
  process.exit(1);
}
let failures = 0;
cases.forEach((text, index) => {
  for (const [offset, method] of [[0, 'toUpperCase'], [1, 'toLowerCase']]) {
    const expected = hex(text[method]());
    const actual = output[index * 2 + offset];
    if (actual !== expected && failures++ < 10) {
      console.error(`${method}(${JSON.stringify(text)}): expected ${expected}, got ${actual}`);
    }
  }
});
if (failures) { console.error(`${failures} mismatches`); process.exit(1); }
console.log(`js case: ${cases.length} strings match Node.js ${process.versions.node} (Unicode ${process.versions.unicode})`);
