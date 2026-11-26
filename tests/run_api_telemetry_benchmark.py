#!/usr/bin/env python3
"""Compare a committed telemetry baseline and the working tree with identical flags.

Uses only local sources. Interleaved repeated runs reduce warm-up/order bias.
Run without other builds or load generators; timings never gate correctness.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, choices=range(3, 11), default=3)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    snapshot = out / 'baseline'
    snapshot.mkdir(exist_ok=True)
    ref = subprocess.check_output(['git', 'rev-parse', '--verify', args.baseline_ref], cwd=ROOT, text=True).strip()
    for name in ('api_telemetry.cpp', 'api_telemetry.h'):
        (snapshot / name).write_bytes(subprocess.check_output(['git', 'show', f'{ref}:src/operations/{name}'], cwd=ROOT))
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    flags = ['-O2', '-std=c++23', '-fno-rtti', '-ffp-contract=off', '-pthread',
             '-Wall', '-Wextra', '-Wpedantic', '-Werror']
    includes = ['-I' + str(path) for path in sorted((ROOT / 'src').iterdir()) if path.is_dir()]
    includes += ['-Ithird_party/nanopb', '-Iproto/generated', '-I.deps/mbedtls-3.6.7/include']
    common = ['tests/api_telemetry_benchmark.cpp', 'build/price_history_batch_codec.o', 'build/proto_wire.o']
    commands = {}
    input_files = ['src/operations/api_telemetry.cpp', 'src/operations/api_telemetry.h',
                   'src/operations/telemetry_aggregate.h', 'tests/api_telemetry_benchmark.cpp']
    input_hashes = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in input_files}
    for name, source, extra in (
            ('baseline', snapshot / 'api_telemetry.cpp', ['-DFORTIS_TELEMETRY_BASELINE', '-I' + str(snapshot)]),
            ('candidate', ROOT / 'src/operations/api_telemetry.cpp', [])):
        command = compiler + flags + extra + includes + [str(source)] + common + ['-o', str(out / name / 'benchmark' if name == 'baseline' else out / 'candidate-benchmark')]
        subprocess.run(command, cwd=ROOT, check=True)
        commands[name] = command
    results = {'baseline': [], 'candidate': []}
    for repeat in range(args.repeats):
        for name in (('baseline', 'candidate') if repeat % 2 == 0 else ('candidate', 'baseline')):
            executable = snapshot / 'benchmark' if name == 'baseline' else out / 'candidate-benchmark'
            raw = subprocess.check_output([str(executable)], cwd=ROOT, text=True, timeout=180)
            (out / f'{name}-{repeat}.jsonl').write_text(raw)
            results[name].extend(dict(json.loads(line), repeat=repeat) for line in raw.splitlines())
    summary = []
    for saturated, writers, polling in itertools.product((False, True), (1, 4), (False, True)):
        row = {'writers': writers, 'polling': polling, 'deviceSetSaturated': saturated}
        for name in results:
            values = [r for r in results[name] if r.get('writers') == writers and r['polling'] == polling and r['deviceSetSaturated'] == saturated]
            row[name] = {key: statistics.median(r[key] for r in values)
                         for key in ('recordsPerSec', 'p50Us', 'p95Us', 'p99Us', 'renders')}
        row['throughputRatio'] = round(row['candidate']['recordsPerSec'] / row['baseline']['recordsPerSec'], 2)
        summary.append(row)
    assert all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == h for p, h in input_hashes.items()), 'Inputs changed during measurement'
    report = {'inputSha256': input_hashes, 'baselineRef': ref, 'host': platform.platform(), 'machine': platform.machine(),
              'compiler': subprocess.check_output(compiler + ['--version'], text=True).splitlines()[0],
              'timestampUtc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'commands': commands, 'repeats': args.repeats, 'results': results, 'medianSummary': summary,
              'limitations': ['Component Record throughput, not end-to-end API capacity',
                              'Closed-loop synthetic workload, 96 routes, 15 populated minutes, ordinary and saturated device sets',
                              'Continuous render pressure exceeds normal browser polling',
                              'Every 67th Record latency sampled with per-writer offset; timing is environment-dependent']}
    (out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
