#!/usr/bin/env node
// Maintenance tool, NOT part of the build or of `make test`.
//
// Records what this Node.js runtime answers for String.prototype.toUpperCase
// and toLowerCase into tests/corpus/js_case_expectations.json (and its
// .sha256 sidecar). tests/js_case_test.py replays that file against
// src/compat/js_case.cpp, so verifying the project needs no JavaScript
// engine. Run it again only when the Unicode tables are regenerated
// (tests/reference/generate_js_case_tables.mjs) for a newer engine:
//
//     node tests/reference/record_js_case.mjs
//
// The cases are those of the earlier live test (JavaScript version of this test): every
// Unicode scalar value alone, the hand-written Final_Sigma and special-casing
// contexts, and 20000 seeded mixed strings. The mixed strings come from a
// corrected generator (see below); the earlier one produced 29 different
// strings. To keep the file
// small, a scalar value is written only when upper- or lower-casing changes
// it; the Python test derives the identity cases for all the others.
import { createHash } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const target = path.join(root, 'tests/corpus/js_case_expectations.json');

// --- scalar values and contexts, as in JavaScript version of this test --------------
const cases = [];
for (let cp = 0; cp <= 0x10ffff; cp++) {
  if (cp >= 0xd800 && cp <= 0xdfff) continue;
  cases.push(String.fromCodePoint(cp));
}
const scalarCount = cases.length;
const sigma = ['Σ', 'ΣΣ', 'AΣ', 'AΣB', 'AΣ.', 'AͅΣ', 'AΣͅ', 'AΣͅB', 'A.Σ',
  'A\'Σ', 'ΑΣ ΤΟΣ', 'ΌΣΟΣ', '1Σ', 'AΣ1', 'ὈΔΥΣΣΕΎΣ', 'A­Σ­', 'aΣ‍',
  'İSTANBUL', 'ß straße', 'ŉ', 'ǰ', 'ﬁ', 'ΐ', 'ᾳ', 'ᾼ', 'Ǆǅǆ', '\u{10400}\u{10428}'];
cases.push(...sigma);
// Seeded mixed strings. The earlier test computed
// `seed = (seed * 1103515245 + 12345) & 0x7fffffff` in double arithmetic: the
// product exceeds 2^53, the low bits are lost, and its 20000 strings were only
// 29 different ones, nearly all a single character. This generator is exact
// in 32 bits (Math.imul) and takes its choices from the high bits.
let seed = 7;
const random = () => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed >>> 16; };
const oneOf = (list) => list[random() % list.length];
// The earlier test's pool.
const pool = ['A', 'a', '\u03a3', '\u03c3', '\u03c2', '\u0345', '.', '\'', ' ', '\u00ad', '\u00df', '\u0130', '\u0131',
  '\u{1e900}', '\u{10400}', '\u03a9', '\u01c5', '\u0307', '0', '\u{1f600}'];
// Final_Sigma decides between two lower-case forms of capital sigma from what
// surrounds it: a cased letter before, none after, case-ignorable characters
// skipped on both sides. So the strings mix these four kinds.
const SIGMA = ['\u03a3', '\u03a3', '\u03a3', '\u03c3', '\u03c2', '\u03f2', '\u03f9', '\u{1d6ba}'];
const CASED = [
  'A', 'a', 'Z', 'z', 'I', 'i', 'K', 's', 'N',
  // Latin with special or one-to-many mappings: sharp s (both cases), dotted
  // and dotless i, long s, n-apostrophe, j-caron, micro, Kelvin, Angstrom.
  '\u00df', '\u1e9e', '\u0130', '\u0131', '\u017f', '\u0149', '\u01f0', '\u00b5', '\u212a', '\u212b', '\u00e9', '\u00c9', '\u00ff', '\u0178',
  // Title-case digraphs and their upper/lower forms.
  '\u01c4', '\u01c5', '\u01c6', '\u01c7', '\u01c8', '\u01c9', '\u01f1', '\u01f2', '\u01f3',
  // Greek: accented, with iota subscript, final-position forms.
  '\u0391', '\u03b1', '\u03a9', '\u03c9', '\u038c', '\u03cc', '\u0390', '\u03b0', '\u1fb3', '\u1fbc', '\u1f80', '\u1f88', '\u1fb7', '\u1ff7', '\u1f50', '\u1fd3', '\u03d0', '\u03d1', '\u03f4',
  // Cyrillic, Armenian (with ligatures), Georgian (three scripts), Cherokee.
  '\u0416', '\u0436', '\u0401', '\u0451', '\u1c80', '\u0531', '\u0561', '\u0587', '\ufb13', '\u10d0', '\u1c90', '\u10a0', '\u2d00', '\u13a0', '\uab70', '\u13f8',
  // Latin ligatures, Roman numerals, circled and full-width letters.
  '\ufb00', '\ufb01', '\ufb03', '\ufb06', '\u2160', '\u2170', '\u24b6', '\u24d0', '\uff21', '\uff41', '\ua64a', '\ua7ae', '\u2c6f', '\u0250',
  // Outside the Basic Multilingual Plane: Deseret, Osage, Old Hungarian,
  // Warang Citi, Medefaidrin, Adlam, Garay; mathematical letters are cased
  // but have no mapping.
  '\u{10400}', '\u{10428}', '\u{104b0}', '\u{104d8}', '\u{10c80}', '\u{10cc0}', '\u{118a0}', '\u{118c0}', '\u{16e40}', '\u{16e60}',
  '\u{1e900}', '\u{1e922}', '\u{10d50}', '\u{10d70}', '\u{1d400}', '\u{1d41a}', '\u{1f130}',
];
// Case-ignorable: skipped when Final_Sigma looks for a cased neighbour. Some
// are also cased (modifier letters, combining ypogegrammeni).
const IGNORABLE = ['.', '\'', ':', '\u00b7', '\u2019', '\u00ad', '\u0307', '\u0301', '\u0308', '\u0345', '\u200b', '\u200d', '\u2060', '\ufeff',
  '\u02b0', '\u02bc', '\u1d2c', '\u00aa', '\u0387', '\u055a', '\u180e', '\u{1f3fb}', '\u{e0061}', '\u{e0100}', '\u{1d167}', '\u{16f8f}', '^', '`', '\u00a8'];
// Neither cased nor case-ignorable: these end a Final_Sigma context.
const OTHER = [' ', '0', '9', '-', '_', ',', '!', '/', '\n', '\t', '\u0000', '\u00a0', '\u2028', '\u2029', '\u4e2d', '\u3042', '\u05d0', '\u0627',
  '\u{1f600}', '\u{20000}', '\u{10ffff}', '\ufffd', '\uffff', '\u2603', '\u00d7', '\u00f7'];
const mixed = [];
for (let i = 0; i < 20000; i++) {
  let text = '';
  const length = 1 + random() % 12;
  for (let j = 0; j < length; j++) {
    const kind = random() % 16;
    text += kind < 3 ? oneOf(pool) : kind < 6 ? oneOf(SIGMA) : kind < 11 ? oneOf(CASED)
      : kind < 14 ? oneOf(IGNORABLE) : oneOf(OTHER);
  }
  mixed.push(text);
}
cases.push(...mixed);
// ---------------------------------------------------------------------------

// A JavaScript string is UTF-16 and may hold a lone surrogate. None of these
// cases or answers has one today. Should one be added: JSON.stringify writes
// it as an escape (\udXXX), Python's json module reads that back as the same
// lone surrogate, and both sides turn it into U+FFFD (ef bf bd) when making
// the UTF-8 hex for the driver, exactly as Buffer.from(text, 'utf8') does
// below. The stdinSha256 check in the Python test proves the bytes agree.

const hex = (text) => Buffer.from(text, 'utf8').toString('hex');
// The bytes to write to the driver's standard input, encoded as the earlier
// test did ("U <hex>" and "L <hex>" per string, hex of Buffer.from(text,
// 'utf8')). The Python test builds the same bytes and compares the hash before
// trusting its own derivation of the identity cases.
const stdin = createHash('sha256');
for (const text of cases) stdin.update(`U ${hex(text)}\nL ${hex(text)}\n`);

const code = (cp) => cp.toString(16).toUpperCase().padStart(4, '0');
const row = (values) => `    ${JSON.stringify(values)}`;
const changed = [];
for (let i = 0; i < scalarCount; i++) {
  const text = cases[i];
  const upper = text.toUpperCase(), lower = text.toLowerCase();
  if (upper !== text || lower !== text) changed.push(row([code(text.codePointAt(0)), upper, lower]));
}
const triple = (text) => row([text, text.toUpperCase(), text.toLowerCase()]);

const header = {
  about: 'Recorded answers of String.prototype.toUpperCase / toLowerCase, replayed by ' +
    'tests/js_case_test.py against src/compat/js_case.cpp. Do not edit by hand.',
  engine: `Node.js ${process.versions.node} (V8 ${process.versions.v8}, ICU ${process.versions.icu})`,
  node: process.versions.node,
  unicode: process.versions.unicode,
  recorder: 'tests/reference/record_js_case.mjs',
  regenerate: 'node tests/reference/record_js_case.mjs',
  layout: {
    scalars: '[code point in hex, upper-cased, lower-cased] for every Unicode scalar value ' +
      '(U+0000..U+10FFFF without the surrogates U+D800..U+DFFF) that either operation changes. ' +
      'Every scalar value not listed is returned unchanged by both operations.',
    contexts: '[input, upper-cased, lower-cased]: Final_Sigma and special-casing contexts.',
    mixed: '[input, upper-cased, lower-cased]: the seeded corpus of mixed strings.',
  },
  strings: cases.length,
  stdinSha256: stdin.digest('hex'),
};
const body = `{\n  "source": ${JSON.stringify(header, null, 2).replace(/\n/g, '\n  ')},\n` +
  `  "scalars": [\n${changed.join(',\n')}\n  ],\n` +
  `  "contexts": [\n${sigma.map(triple).join(',\n')}\n  ],\n` +
  `  "mixed": [\n${mixed.map(triple).join(',\n')}\n  ]\n}\n`;
JSON.parse(body);
writeFileSync(target, body);
writeFileSync(`${target}.sha256`, `${createHash('sha256').update(body).digest('hex')}\n`);
console.log(`recorded ${cases.length} strings (${changed.length} scalar values that change, ` +
  `${sigma.length} contexts, ${mixed.length} mixed) from Node.js ${process.versions.node} ` +
  `(Unicode ${process.versions.unicode}) into ${path.relative(root, target)}`);
