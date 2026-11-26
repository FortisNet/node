#!/usr/bin/env python3
"""Retain actual GCC/LTO stack reports alongside a static release.

Per-frame reports are measurements, not a whole-call-chain/recursion proof.
Indirect calls, external library frames and runtime workload coverage still need
qualification. Fail closed if the actual final link produced no measurements.
"""
import argparse
import json
import math
from pathlib import Path
import re
import shutil


def thread_stacks(source):
    """Inventory current explicit stack expressions; do not infer call graphs."""
    inventory = []
    for path in sorted(source.rglob('*.cpp')) + [source / 'base/threading.h']:
        text = path.read_text()
        patterns = (r'pthread_attr_setstacksize\([^,]+,\s*([^)]*)\)',
                    r'\.Start\([^;\n]*?,\s*(\d+(?:\s*\*\s*\d+)+)\)')
        for pattern in patterns:
            for match in re.finditer(pattern, text):
                expression = match[1].strip()
                if re.fullmatch(r'[A-Za-z_]\w*', expression):
                    constant = re.search(r'constexpr size_t\s+' + expression + r'\s*=\s*([^;]+);', text)
                    if constant:
                        expression = constant[1].strip()
                    elif path.name == 'threading.h' and expression == 'stack_bytes':
                        continue  # Generic forwarding API; literal callers appear above.
                if not re.fullmatch(r'\d+(?:\s*\*\s*\d+)*', expression):
                    raise ValueError(f'Unreviewed explicit thread stack: {path.name}: {expression}')
                inventory.append({'source': str(path.relative_to(source.parent)),
                                  'line': text.count('\n', 0, match.start()) + 1,
                                  'bytes': math.prod(int(term.strip()) for term in expression.split('*'))})
    return inventory


def validate_frames(frames):
    """Bound single frames; never mistake this check for a whole-stack proof."""
    for frame in frames:
        function = frame['function']
        owner_completion = (function.startswith('src/prices/spot_owner_batches.cpp:') and
                            function.split(':', 3)[-1].startswith('CompleteBatch'))
        # This completion runs only on the explicitly 1 MiB spot owner.
        ceiling = 384 * 1024 if owner_completion else 96 * 1024
        if frame['bytes'] > ceiling or ('dynamic' in frame['kind'] and 'bounded' not in frame['kind']):
            raise ValueError(f'Unqualified release stack frame: {function}: {frame["bytes"]} {frame["kind"]}')


def collect(build, output):
    # Test executables have their own LTO output: they cannot qualify the server.
    reports = sorted(build.glob('fortis_node*.su'))
    frames = []
    output.mkdir(parents=True, exist_ok=True)
    for index, path in enumerate(reports):
        shutil.copyfile(path, output / f'{index:04d}-{path.name}')
        for line in path.read_text().splitlines():
            fields = line.rsplit('\t', 2)
            if len(fields) != 3 or not fields[1].isdigit():
                raise ValueError(f'Malformed stack report: {path.name}')
            frames.append({'function': fields[0], 'bytes': int(fields[1]), 'kind': fields[2]})
    if not frames:
        raise ValueError('No release-toolchain stack usage records; cannot qualify stack budgets')
    for path in sorted(build.glob('fortis_node*.ci')):
        shutil.copyfile(path, output / path.name)
    frames.sort(key=lambda row: (-row['bytes'], row['function']))
    validate_frames(frames)
    result = {'compiler': 'GCC 15.2.0 musl static size/LTO', 'frames': frames,
              'maximum_frame_bytes': frames[0]['bytes'],
              'frame_ceilings': {'default_bytes': 96 * 1024, 'spot_owner_completion_bytes': 384 * 1024},
              'explicit_thread_stacks': thread_stacks(Path(__file__).resolve().parents[1] / 'src'),
              'limitations': ['Per-frame, not an inferred whole-stack high-water mark',
                              'Indirect calls, recursion and external-library frames require separate review']}
    (output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"Stack usage: {len(frames)} frames; largest {frames[0]['bytes']} bytes")
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    collect(args.build, args.output)
