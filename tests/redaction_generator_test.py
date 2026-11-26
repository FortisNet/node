#!/usr/bin/env python3
"""Check how the redaction table generator compares two generated files.

A differing ICU build version in the provenance comment must not count as a
change; a differing table value, Unicode version or rules digest must.
"""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'redaction_tables_generator',
    Path(__file__).resolve().parents[1] / 'scripts/generate_redaction_tables.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
same = generator.same_generated_tables


def check(condition, case):
    if not condition:
        raise AssertionError(case)


table = ('// Unicode 17.0 normalization data of Node.js (ICU 78.3).\n'
         '// Rules digest: original\nconst int values[] = {1, 2};\n')
check(same(table, table), 'identical tables are the same')
check(same(table, table.replace('ICU 78.3', 'ICU 78.2')), 'ICU version is ignored')
check(not same(table, table.replace('{1, 2}', '{1, 3}')), 'table value is compared')
check(not same(table, table.replace('Unicode 17.0', 'Unicode 16.0')),
      'Unicode version is compared')
check(not same(table, table.replace('digest: original', 'digest: changed')),
      'rules digest is compared')
print('redaction generator comparison: 5 cases passed')
