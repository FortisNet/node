#!/usr/bin/env python3
"""Merge independent CI gates; incomplete, failed or mixed-input groups fail closed."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import verify

GROUPS = {'native': verify.NATIVE_STAGES,
          **{stage: (stage,) for stage in ('clang-analyzer', 'lint', 'cppcheck')}}
CLEAN_DIFF = hashlib.sha256(b'').hexdigest()


def merge(directory, commit):
    """Require exact stage coverage and identical clean source/tool snapshots."""
    merged = {'complete': False, 'stages': {}, 'commit': commit, 'groups': {}}
    reference = None
    native_directory = None
    native_report = None
    for group, stages in GROUPS.items():
        paths = list(directory.rglob(f'report-{group}.json'))
        if len(paths) != 1:
            raise ValueError(f'{group}: expected exactly one report, found {len(paths)}')
        report = json.loads(paths[0].read_text())
        if not isinstance(report, dict) or report.get('complete') is not True:
            raise ValueError(f'{group}: incomplete report')
        if report.get('inputs_unchanged') is not True or report.get('bootstrap', {}).get('returncode') != 0:
            raise ValueError(f'{group}: changed inputs or failed bootstrap')
        if report.get('commit') != commit or report.get('diff_sha256') != CLEAN_DIFF:
            raise ValueError(f'{group}: wrong commit or dirty checkout')
        actual = report.get('stages', {})
        if set(actual) != set(stages) or any(row.get('returncode') != 0 for row in actual.values()):
            raise ValueError(f'{group}: missing, unexpected or failed stages')
        snapshot = (report.get('input_sha256'), report.get('tool_versions'))
        if not all(isinstance(value, dict) and value for value in snapshot):
            raise ValueError(f'{group}: missing source/tool snapshot')
        if reference is not None and snapshot != reference:
            raise ValueError(f'{group}: source or tools differ between jobs')
        reference = snapshot
        merged['stages'].update(actual)
        merged['groups'][group] = {'report_sha256': hashlib.sha256(paths[0].read_bytes()).hexdigest(),
                                   'started_at': report.get('started_at')}
        if group == 'native':
            native_directory, native_report = paths[0].parent, report
    if set(merged['stages']) != set(verify.STAGES):
        raise ValueError('aggregate does not cover all seven stages')
    results = {}
    for stage, name in verify.RESULT_FILES.items():
        data = (native_directory / name).read_bytes()
        rows = json.loads(data)
        expected = native_report.get('test_results', {}).get(stage)
        digest = {'sha256': hashlib.sha256(data).hexdigest(), 'count': len(rows)}
        if expected != digest or not isinstance(rows, list) or not rows:
            raise ValueError(f'{stage}: missing, empty or altered test results')
        commands = {}
        for row in rows:
            if not isinstance(row, dict) or row.get('returncode') != 0:
                raise ValueError(f'{stage}: failed or blocked test')
            command = row.get('command')
            if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
                raise ValueError(f'{stage}: invalid command')
            compiler = row.get('compiler') if stage == 'unit-tests' else 'clang'
            command_set = commands.setdefault(compiler, set())
            if tuple(command) in command_set:
                raise ValueError(f'{stage}: duplicate command')
            command_set.add(tuple(command))
        if stage == 'unit-tests' and (set(commands) != {'clang', 'gcc'} or commands['clang'] != commands['gcc']):
            raise ValueError('unit-tests: compiler families have different coverage')
        results[name] = data
    merged.update(complete=True, inputs_unchanged=True, input_sha256=reference[0],
                  tool_versions=reference[1], diff_sha256=CLEAN_DIFF, bootstrap={'returncode': 0},
                  test_results=native_report['test_results'])
    return merged, results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--jobs-result', choices=('success', 'failure', 'cancelled', 'skipped'), default='success')
    parser.add_argument('--output', type=Path, default=Path('build-verify'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        if args.jobs_result != 'success':
            raise ValueError(f'CI jobs did not all succeed: {args.jobs_result}')
        report, results = merge(args.directory, args.commit)
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        (args.output / 'report.json').write_text(json.dumps({'complete': False, 'error': str(error)}, indent=2) + '\n')
        print(f'verify CI: {error}', file=sys.stderr)
        return 1
    for name, data in results.items():
        (args.output / name).write_bytes(data)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('verify CI: PASS; all seven stages, both compiler families and matching unchanged inputs')
    return 0


if __name__ == '__main__':
    sys.exit(main())
