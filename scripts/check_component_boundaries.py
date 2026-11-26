#!/usr/bin/env python3
"""Enforce explicit header exports and the shared host/guest component partition.

Public headers may include their own private layouts for allocation-free value
composition. Consumers must use public entry points; direct cross-module private
includes are forbidden. Existing layer/namespace checks remain authoritative.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import subprocess
import tempfile

INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')

def no_comments(text):
    return TOKENS.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]) if m[0].startswith(('//', '/*')) else m[0], text)

def check_headers(root, manifest):
    errors, entries, names = [], {}, {}
    for module, groups in manifest['modules'].items():
        for visibility in ('public', 'private'):
            for path in groups[visibility]:
                if path in entries:
                    errors.append('header classified twice: ' + path)
                if Path(path).parts[0] != module:
                    errors.append('header belongs to another module: ' + path)
                entries[path] = visibility
                names[Path(path).name] = path
    actual = {str(p.relative_to(root / 'src')) for p in (root / 'src').glob('*/*') if p.suffix in ('.h', '.inc')}
    for path in sorted(actual - entries.keys()):
        errors.append('unclassified header: ' + path)
    for path in sorted(entries.keys() - actual):
        errors.append('stale header classification: ' + path)
    for source in sorted((root / 'src').glob('*/*')):
        if source.suffix not in ('.h', '.cpp', '.c', '.inc'):
            continue
        owner = source.parent.name
        for include in INCLUDE.findall(no_comments(source.read_text())):
            if Path(include).name == 'native_config.h' and owner not in ('app', 'config'):
                errors.append(str(source.relative_to(root / 'src')) + ' consumes process-wide configuration')
            target = names.get(Path(include).name)
            if target and entries[target] == 'private' and Path(target).parts[0] != owner:
                errors.append(str(source.relative_to(root / 'src')) + ' -> private ' + target)
    return errors

def check_partition(sources, groups):
    errors = []
    counts = collections.Counter(p for paths in groups.values() for p in paths)
    for source in sources:
        if counts[source] != 1:
            errors.append(f'component ownership {counts[source]} times: {source}')
    for source in counts.keys() - set(sources):
        errors.append('component names non-production source: ' + source)
    for component, paths in groups.items():
        for path in paths:
            if path.startswith('src/'):
                expected = Path(path).parts[1]
            elif path.startswith(('third_party/nanopb/', 'proto/generated/')):
                expected = 'codec'
            elif path.startswith(('third_party/blst/', 'third_party/blake3/')):
                expected = 'crypto'
            else:
                expected = None
            if component != expected:
                errors.append(f'{component} owns {path} instead of {expected}')
    return errors

def self_test():
    with tempfile.TemporaryDirectory(prefix='fortis-boundary-unit-') as tmp:
        root = Path(tmp)
        for module in ('base', 'codec'):
            (root / 'src' / module).mkdir(parents=True)
        (root / 'src/base/public.h').write_text('#pragma once\n#include "detail_internal.h"\n')
        (root / 'src/base/detail_internal.h').write_text('#pragma once\n')
        client = root / 'src/codec/client.cpp'
        client.write_text('#include "public.h"\n')
        manifest = {'modules': {'base': {'public': ['base/public.h'], 'private': ['base/detail_internal.h']},
                                'codec': {'public': [], 'private': []}}}
        assert not check_headers(root, manifest)
        client.write_text('#include <base/detail_internal.h>\n')
        assert any('-> private' in e for e in check_headers(root, manifest))
        client.write_text('/*\n#include "detail_internal.h"\n*/\n#include "public.h"\n')
        assert not check_headers(root, manifest)
        client.write_text('#include \"native_config.h\"\n')
        assert any('process-wide configuration' in e for e in check_headers(root, manifest))
        client.write_text('#include \"public.h\"\n')
        (root / 'src/base/new.h').write_text('#pragma once\n')
        assert any('unclassified' in e for e in check_headers(root, manifest))
        (root / 'src/base/new.h').unlink()
        (root / 'src/base/detail_internal.h').unlink()
        assert any('stale' in e for e in check_headers(root, manifest))
    sources = ['src/base/a.cpp', 'proto/generated/a.c']
    assert not check_partition(sources, {'base': [sources[0]], 'codec': [sources[1]]})
    assert check_partition(sources, {'base': [sources[0], sources[0]], 'codec': [sources[1]]})
    assert check_partition(sources, {'base': []})
    assert check_partition(sources, {'crypto': [sources[0]], 'codec': [sources[1]]})
    backend = ['src/crypto/blst_platform_cap.c', 'third_party/blst/x/server.c']
    assert not check_partition(backend, {'crypto': backend})
    assert check_partition(backend, {'crypto': backend[:1], 'codec': backend[1:]})
    print('component boundary self-tests: 12 cases passed')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / 'component_headers.json').read_text())
    errors = check_headers(root, manifest)
    shared = (root / 'sources.mk').read_text()
    sources = re.findall(r'^FORTIS_SOURCES \+= (\S+)$', shared, re.M)
    output = subprocess.check_output(['make', '--no-print-directory', '-s', 'print-component-sources'], cwd=root, text=True)
    groups = {}
    for line in output.splitlines():
        component, paths = line.split(':', 1)
        if component in groups:
            errors.append('duplicate component: ' + component)
        groups[component] = paths.split()
    errors += check_partition(sources, groups)
    if set(groups) != set(manifest['modules']):
        errors.append('component set differs from module header inventory')
    if errors:
        for error in errors:
            print('FAIL: ' + error)
        return 1
    public = sum(len(v['public']) for v in manifest['modules'].values())
    private = sum(len(v['private']) for v in manifest['modules'].values())
    print(f'component boundaries: {public} public, {private} private headers; {len(groups)} components, {len(sources)} sources, exact partition')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
