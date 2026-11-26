import assert from 'node:assert/strict';
import { sameGeneratedTables } from '../scripts/generate_redaction_tables.mjs';

const table = '// Unicode 17.0 normalization data of Node.js (ICU 78.3).\n'
  + '// Rules digest: original\nconst int values[] = {1, 2};\n';
assert.ok(sameGeneratedTables(table, table));
assert.ok(sameGeneratedTables(table, table.replace('ICU 78.3', 'ICU 78.2')));
assert.ok(!sameGeneratedTables(table, table.replace('{1, 2}', '{1, 3}')));
assert.ok(!sameGeneratedTables(table, table.replace('Unicode 17.0', 'Unicode 16.0')));
assert.ok(!sameGeneratedTables(table, table.replace('digest: original', 'digest: changed')));
console.log('redaction generator comparison: 5 cases passed');
