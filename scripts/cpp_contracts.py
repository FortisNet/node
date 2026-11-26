#!/usr/bin/env python3
"""Incremental enforced formatting, public naming, and generated contract docs.

Scope is explicit in config/cpp_contracts.json. This is not a blanket legacy
warning baseline. Existing full-project layering/ownership/lint checks remain.
"""
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def naming_config():
    return {'Checks': '-*,readability-identifier-naming', 'WarningsAsErrors': '*',
              'CheckOptions': {'readability-identifier-naming.ClassCase': 'CamelCase',
                               'readability-identifier-naming.StructCase': 'CamelCase',
                               'readability-identifier-naming.FunctionCase': 'CamelCase',
                               'readability-identifier-naming.MethodCase': 'CamelCase',
                               'readability-identifier-naming.MethodIgnoredRegexp': '^(data|chars|bytes|size|reset|release|get|view|capacity|c_str|value|ok|status|code|message|lock|unlock|try_lock|native|started|locked)$'}}


def main():
    policy = json.loads((ROOT / 'config/cpp_contracts.json').read_text())
    output = ROOT / 'build/cpp-contracts-docs'
    output.mkdir(parents=True, exist_ok=True)
    format_tool = os.environ.get('CLANG_FORMAT', 'clang-format-18')
    tidy = os.environ.get('CLANG_TIDY', 'clang-tidy-18')
    subprocess.run([format_tool, '--dry-run', '--Werror', *policy['format']], cwd=ROOT, check=True)
    naming = naming_config()
    for name in policy['naming']:
        subprocess.run([tidy, name, '-config=' + json.dumps(naming), '--', '-x', 'c++-header', '-std=c++23',
                        '-I' + str(ROOT / 'src/base')], cwd=ROOT, check=True)
    config = '\n'.join([
        'PROJECT_NAME = "Fortis C++ ownership contracts"', 'OUTPUT_DIRECTORY = ' + str(output),
        'INPUT = ' + ' '.join(policy['document']), 'RECURSIVE = NO', 'EXTRACT_ALL = YES',
        'EXTRACT_PRIVATE = NO', 'GENERATE_HTML = YES', 'GENERATE_XML = YES', 'GENERATE_LATEX = NO',
        'QUIET = YES', 'WARN_AS_ERROR = YES', 'WARN_IF_DOC_ERROR = YES', 'WARN_IF_UNDOCUMENTED = NO',
        'WARN_LOGFILE = ' + str(output / 'warnings.log'), 'ENABLE_PREPROCESSING = YES',
        'HAVE_DOT = NO', 'CLASS_GRAPH = NO', 'MACRO_EXPANSION = NO', 'FULL_PATH_NAMES = NO', 'FILE_PATTERNS = *.h'])
    (output / 'Doxyfile').write_text(config + '\n')
    generated = subprocess.run(['doxygen', str(output / 'Doxyfile')], cwd=ROOT, capture_output=True, text=True)
    if generated.returncode or generated.stderr.strip():
        raise RuntimeError('documentation generation failed: ' + generated.stdout + generated.stderr)
    index = ET.parse(output / 'xml/index.xml')
    names = {c.findtext('name') for c in index.findall('compound')}
    if not set(policy['documented_types']).issubset(names):
        raise RuntimeError('generated documentation omitted contract types: ' + str(set(policy['documented_types']) - names))
    warnings = (output / 'warnings.log').read_text()
    if warnings.strip():
        raise RuntimeError('documentation warnings: ' + warnings)
    print('Scoped C++ formatting, naming and generated documentation passed.')


if __name__ == '__main__':
    main()
