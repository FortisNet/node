import {readFile, writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
const here = dirname(fileURLToPath(import.meta.url));
const rows = JSON.parse(await readFile(resolve(here, '../contracts/price-id-fixtures.json'), 'utf8'));
function cString(value) {
  const bytes = Buffer.from(value, 'utf8');
  let result = '"';
  for (const byte of bytes) {
    if (byte === 0x22) result += '\\"';
    else if (byte === 0x5c) result += '\\\\';
    else if (byte >= 0x20 && byte <= 0x7e) result += String.fromCharCode(byte);
    else result += `\\${byte.toString(8).padStart(3, '0')}`;
  }
  return `${result}"`;
}

function entry(item) {
  return `  {${cString(item.name)}, ${cString(item.raw)}, fortis::providers::PriceIdKind::${item.tsKind}, ` +
    `${cString(item.tsSource)}, ${cString(item.tsNetwork)}, ${cString(item.tsAddress)}, ` +
    `${cString(item.tsStandard)}, fortis::providers::PriceIdKind::${item.nativeKind}, ${cString(item.gap)}},`;
}

const output = resolve(here, '../tests/price_id_parser_fixtures.h');
const header = `// Generated from contracts/price-id-fixtures.json by
// scripts/generate_price_id_parser_fixtures.mjs. Historical reference cases.
#pragma once

#include "../src/providers/price_id_parser.h"

struct PriceIdParserFixture {
  const char* name;
  const char* raw;
  fortis::providers::PriceIdKind typescript_kind;
  const char* typescript_source_id;
  const char* typescript_network;
  const char* typescript_address;
  const char* typescript_standard;
  fortis::providers::PriceIdKind native_kind;
  const char* documented_gap;
};

constexpr PriceIdParserFixture kPriceIdParserFixtures[] = {
${rows.map(entry).join('\n')}
};
`;
await writeFile(output, header);
console.log(`Wrote ${rows.length} fixtures from native-owned reference data to ${output}`);
