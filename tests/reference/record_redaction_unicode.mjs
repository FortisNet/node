#!/usr/bin/env node
// Records contracts/redaction-unicode-data.json: the Unicode normalization
// data the redaction tables are generated from (compatibility decompositions,
// composition pairs and combining ranks), taken from this Node.js runtime's
// ICU. A maintenance tool: run it only to move to a newer Unicode version,
// then regenerate with scripts/generate_redaction_tables.py --write.
//
//   node tests/reference/record_redaction_unicode.mjs
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

function isSurrogate(cp) { return cp >= 0xd800 && cp <= 0xdfff; }
function isHangulSyllable(cp) { return cp >= 0xac00 && cp <= 0xd7a3; }

// Canonical combining classes are not exposed by JavaScript, but their order
// is observable: NFD swaps two adjacent marks exactly when the first has the
// higher class. A code point is a non-starter when it reorders against
// U+0334 (class 1) or U+0301 (class 230). Ranks keep the order and the
// equalities of the real classes, which is all reordering and composition
// blocking compare.
function combiningRanks() {
  const marks = [];
  for (let cp = 0; cp <= 0x10ffff; cp += 1) {
    if (isSurrogate(cp)) continue;
    const mark = String.fromCodePoint(cp);
    if (mark.normalize('NFD') !== mark) continue;
    if (`a${mark}\u0334`.normalize('NFD') === `a\u0334${mark}` ||
        `a\u0301${mark}`.normalize('NFD') === `a${mark}\u0301`) marks.push(mark);
  }
  const after = (x, y) => `a${x}${y}`.normalize('NFD') === `a${y}${x}`;
  const compare = (x, y) => (after(x, y) ? 1 : after(y, x) ? -1 : 0);
  marks.sort((x, y) => compare(x, y) || x.codePointAt(0) - y.codePointAt(0));
  const ranks = [];
  let rank = 0;
  for (let i = 0; i < marks.length; i += 1) {
    if (i === 0 || compare(marks[i - 1], marks[i]) !== 0) rank += 1;
    ranks.push([marks[i].codePointAt(0), rank]);
  }
  if (rank > 255) throw new Error('more combining classes than a byte holds');
  return ranks.sort((a, b) => a[0] - b[0]);
}

function normalizationTables() {
  const map = [];
  const pairs = [];
  for (let cp = 0; cp <= 0x10ffff; cp += 1) {
    if (isSurrogate(cp)) continue;
    const single = String.fromCodePoint(cp);
    // Hangul syllables decompose and compose arithmetically in the native code.
    const nfkd = isHangulSyllable(cp) ? single : single.normalize('NFKD');
    if (nfkd !== single) map.push([cp, [...nfkd].map((char) => char.codePointAt(0))]);
    // Canonical composition pairs: a primary composite is its own NFC and has
    // a two-code-point single-level canonical decomposition. Hangul
    // syllables compose algorithmically in the native code.
    if (isHangulSyllable(cp) || single.normalize('NFC') !== single) continue;
    const nfd = [...single.normalize('NFD')];
    if (nfd.length < 2) continue;
    const second = nfd[nfd.length - 1];
    const first = nfd.slice(0, -1).join('').normalize('NFC');
    if ([...first].length !== 1 || (first + second).normalize('NFC') !== single) continue;
    pairs.push([first.codePointAt(0), second.codePointAt(0), cp]);
  }
  pairs.sort((a, b) => (a[0] - b[0]) || (a[1] - b[1]));
  return { map, pairs, ranks: combiningRanks() };
}


const output = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../contracts/redaction-unicode-data.json');
const tables = normalizationTables();
const rows = (items) => items.map((item) => `    ${JSON.stringify(item)}`).join(',\n');
let text = '{\n';
text += `  "unicodeVersion": ${JSON.stringify(process.versions.unicode)},\n`;
text += `  "icuVersion": ${JSON.stringify(process.versions.icu)},\n`;
const source = `Derived once from the Unicode normalization data built into Node.js ${process.versions.node} `
  + `(ICU ${process.versions.icu}, Unicode ${process.versions.unicode}) by observing String.prototype.normalize for every code point: `
  + 'NFKD mappings (Hangul syllables omitted), primary canonical composition pairs, and canonical combining classes '
  + 'reduced to order-preserving ranks.';
text += `  "source": ${JSON.stringify(source)},\n`;
text += `  "nfkdMappings": [\n${rows(tables.map)}\n  ],\n`;
text += `  "compositionPairs": [\n${rows(tables.pairs)}\n  ],\n`;
text += `  "combiningRanks": [\n${rows(tables.ranks)}\n  ]\n}\n`;
writeFileSync(output, text);
console.log(`recorded ${tables.map.length} mappings, ${tables.pairs.length} pairs, ${tables.ranks.length} ranks from Unicode ${process.versions.unicode}`);
