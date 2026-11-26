#!/usr/bin/env python3
"""Strict pre-push gate. Run make verify (Docker required), or make cppcheck.

Reports and isolated compiler builds live in build-verify/. Every selected stage
runs even if another fails; any failed/blocked stage makes the command fail.
Unit tests are the native ./build/... commands in the authoritative test recipe.
No known-failure baseline, warning baseline or advisory mode is applied.
"""
import argparse
import concurrent.futures
import hashlib
import fcntl
import json
import os
import plistlib
from pathlib import Path
import re
import shlex
import signal
import shutil
import subprocess
import sys
import time

import lint

PROJECT = Path(__file__).resolve().parent.parent
OUTPUT = PROJECT / 'build-verify'
STAGES = ('clang-build', 'clang-analyzer', 'gcc-build', 'lint', 'cppcheck', 'unit-tests', 'integration-tests')
NATIVE_STAGES = ('clang-build', 'gcc-build', 'unit-tests', 'integration-tests')
# `make lean-verify`: one compiler (Clang, warnings as errors, production binary
# check) plus the lint stage. It is the default release gate; the full gate
# adds the analyzers, GCC and the unit/integration suites.
LEAN_STAGES = ('clang-build', 'lint')
RESULT_FILES = {'unit-tests': 'unit-results.json', 'integration-tests': 'integration-results.json'}
UNIT_TIMEOUT_SECONDS = 600
WARNINGS = '-Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wundef -Wcast-align'
CXXFLAGS = f'-O2 -std=c++23 -fno-rtti -ffp-contract=off {WARNINGS} -Woverloaded-virtual -Wnon-virtual-dtor'
CFLAGS = f'-O2 -std=c11 {WARNINGS}'


def run(command, cwd=PROJECT, env=None):
    return subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                          text=True, errors='replace', check=True).stdout


def input_hashes():
    paths = [p for directory in ('src', 'tests', 'scripts', 'contracts', 'proto', 'assets', 'third_party', 'config')
             for p in (PROJECT / directory).rglob('*')
             if p.is_file() and '__pycache__' not in p.parts and p.name != '.DS_Store']
    # The CI workflow sits above this project, at the repository root, and is
    # not part of what this gate builds or checks.
    paths += [PROJECT / name for name in ('Makefile', 'sources.mk', '.clang-tidy',
                                         '.cppcheck-suppressions', 'Dockerfile.verify', 'Dockerfile.native-build',
                                         '.gitignore', '.dockerignore', 'component_headers.json', 'openapi.native.yaml',
                                         'LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md')]
    return {str(p.relative_to(PROJECT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(paths)}


def checkout_provenance():
    try:
        return (run(['git', 'rev-parse', 'HEAD']).strip(),
                hashlib.sha256(subprocess.check_output(['git', 'diff', 'HEAD'], cwd=PROJECT,
                                                      stderr=subprocess.DEVNULL)).hexdigest())
    except subprocess.CalledProcessError:
        return None, None  # Standalone source archives have no Git metadata.


def docker(stage, jobs):
    recipe = PROJECT / 'Dockerfile.verify'
    tag = 'fortis-node-verify:' + hashlib.sha256(recipe.read_bytes()).hexdigest()[:12]
    if subprocess.run(['docker', 'image', 'inspect', tag], capture_output=True).returncode:
        with recipe.open('rb') as source:
            subprocess.run(['docker', 'build', '-t', tag, '-'], stdin=source, check=True)
    # The project owns all its generator inputs; nothing is fetched for them.
    command = ['docker', 'run', '--rm', '--init', '-v', f'{PROJECT}:/work',
               '-w', '/work', '-e', 'FORTIS_VERIFY_CONTAINER=1',
               '-e', 'PYTHONDONTWRITEBYTECODE=1']
    commit, diff = checkout_provenance()
    if commit:
        command += ['-e', f'FORTIS_VERIFY_COMMIT={commit}', '-e', f'FORTIS_VERIFY_DIFF_SHA256={diff}']
    if sys.platform.startswith('linux'):
        command += ['--user', f'{os.getuid()}:{os.getgid()}', '-e', 'HOME=/tmp']
    return subprocess.run(command + [tag, 'python3', 'scripts/verify.py',
                                    '--stage', stage, '--jobs', str(jobs)]).returncode


def prepare_tree(compiler):
    tree = OUTPUT / compiler
    tree.mkdir(parents=True, exist_ok=True)
    driver = 'clang++-18' if compiler == 'clang' else 'g++-13'
    signature = hashlib.sha256((PROJECT / 'Dockerfile.verify').read_bytes() +
                               run([driver, '--version']).encode()).hexdigest()
    stamp = tree / '.toolchain'
    if not stamp.exists() or stamp.read_text() != signature:
        # Make tracks flags, but a tool image can change while compiler paths
        # stay the same. Never reuse old objects under a different toolchain.
        build = tree / 'build'
        if build.is_symlink():
            raise RuntimeError(f'refusing to clear a symlinked build directory: {build}')
        if build.exists():
            shutil.rmtree(build)
        stamp.write_text(signature)
    excluded = {'build', '.deps', '.unikraft', '.kraftkit', 'dist', 'data', '__pycache__'}
    for entry in PROJECT.iterdir():
        if entry.name in excluded or entry.name.startswith('build-'):
            continue
        link = tree / entry.name
        if not link.exists() and not link.is_symlink():
            link.symlink_to(Path('../..') / entry.name)
    (tree / '.deps').mkdir(exist_ok=True)
    archive = PROJECT / '.deps/mbedtls-3.6.7.tar.bz2'
    if archive.is_file():
        target = tree / '.deps' / archive.name
        if not target.exists():
            shutil.copyfile(archive, target)
    return tree


def make_command(compiler, jobs):
    clang = compiler == 'clang'
    # Build the server/test prerequisites directly. Contract checks run once in
    # the lint stage; the default `all` target would run them per compiler.
    return ['make', '--no-print-directory', '-k', f'-j{jobs}',
            f'CC={"clang-18" if clang else "gcc-13"}',
            f'CXX={"clang++-18" if clang else "g++-13"}',
            f'CFLAGS={CFLAGS}', f'CXXFLAGS={CXXFLAGS}{" -stdlib=libc++" if clang else ""}']


def native_commands(tree, jobs):
    lines = run(make_command(tree.name, jobs) + ['-n', '-o', 'test-prereqs', 'test'], cwd=tree)
    commands = [shlex.split(line.strip()) for line in lines.splitlines()
                if line.strip().startswith('./build/')]
    if not commands or len({tuple(c) for c in commands}) != len(commands):
        raise RuntimeError('empty or duplicate native unit-test selection')
    return commands


def compilation_database(jobs):
    """Reuse per-TU build flags, including test-only switches; fail on gaps."""
    if not (PROJECT / '.deps/mbedtls-3.6.7/include').is_dir():
        subprocess.run(['sh', 'scripts/fetch-mbedtls.sh'], cwd=PROJECT, check=True)
    try:
        per_source = lint.compile_lines()
    except SystemExit as error:
        # lint's CLI helper exits on make failures. Convert that into a stage
        # failure so later checks still run and report.json is finalized.
        raise RuntimeError('cannot extract compilation flags') from error
    units = lint.translation_units(lint.all_files())
    fallback = shlex.split(run(['make', '-s', 'print-compile-flags']))
    entries = []
    for name in units:
        if name.startswith('tests/') and name.endswith('.cpp') and name not in per_source:
            raise RuntimeError(f'no build flags for {name}')
        flags = per_source.get(name, fallback)
        # The original tree's Makefile defaults are platform-neutral; analyze
        # with the same Clang/libc++ family as the isolated Clang build.
        flags = [f for f in flags if f not in ('-Werror', '-stdlib=libc++')]
        if name.endswith('.h'):
            flags = ['-xc++', *flags]
        entries.append({'directory': str(PROJECT), 'file': name,
                        'arguments': ['clang++-18', *flags, '-stdlib=libc++', '-c', name]})
    # C shims with no host counterpart are verified by the guest build, not by
    # pretending host headers implement Unikraft. Host-native C shims are here.
    manifest = run(['make', '-s', 'print-component-sources'])
    sources = {s for line in manifest.splitlines() for s in line.split(':', 1)[-1].split()}
    guest_only = {'src/operations/guest_allocator_stats.c', 'src/operations/guest_socket_stats.c'}
    stamp = run(['make', '-s', 'print-mbedtls-stamp']).strip()
    dry = run(['make', '-n', '-B', '-o', stamp,
               '-o', '.deps/mbedtls-3.6.7/library/Makefile', 'CC=clang-18', 'test-prereqs'])
    c_flags = {}
    for line in dry.splitlines():
        words = shlex.split(line)
        if not words or words[0] != 'clang-18':
            continue
        own = [w for w in words if w.startswith('src/') and w.endswith('.c')]
        if len(own) != 1:
            continue
        flags = []
        skip = False
        for word in words[1:]:
            if skip:
                skip = False
                continue
            if word in ('-o', '-MF', '-MT'):
                skip = True
            elif word not in ('-c', '-MMD', '-MP', own[0]):
                flags.append(word)
        c_flags[own[0]] = flags
    for name in sorted(sources):
        if name.startswith('src/') and name.endswith('.c') and name not in guest_only:
            if name not in c_flags:
                raise RuntimeError(f'no build flags for {name}')
            entries.append({'directory': str(PROJECT), 'file': name,
                            'arguments': ['clang-18', *c_flags[name], '-c', name]})
    path = OUTPUT / 'compile_commands.json'
    if any(flag == '-DNDEBUG' or flag.startswith('-DNDEBUG=')
           for entry in entries for flag in entry['arguments']):
        raise RuntimeError('NDEBUG disables assertions required by the unit tests')
    path.write_text(json.dumps(entries, indent=2) + '\n')
    return entries


def analyze(entries, jobs, logs):
    """Run the Clang Static Analyzer explicitly, even on up-to-date builds."""
    def one(entry):
        name = entry['file']
        args = [f for f in entry['arguments'][1:] if f not in ('-c', name)]
        target = logs / (name.replace('/', '_') + '.plist')
        target.unlink(missing_ok=True)
        command = [entry['arguments'][0], '--analyze', *args, name, '-o', str(target),
                   '-Xanalyzer', '-analyzer-output=plist']
        proc = subprocess.run(command, cwd=PROJECT, capture_output=True, text=True, errors='replace')
        findings, suppressed = [], []
        if target.exists():
            with target.open('rb') as report:
                document = plistlib.load(report)
            for finding in document['diagnostics']:
                location = finding['location']
                filename = Path(document['files'][location['file']])
                if not filename.is_absolute():
                    filename = PROJECT / filename
                line = filename.read_text().splitlines()[location['line'] - 1]
                # Honor only existing, explicitly named analyzer checks on the
                # diagnostic's own line, as clang-tidy does. Never infer a
                # suppression from an arbitrary NOLINT elsewhere in the file.
                marker = re.search(r'NOLINT\(([^)]+)\)', line)
                check = 'clang-analyzer-' + finding['check_name']
                ignored = marker and check in {c.strip() for c in marker[1].split(',')}
                (suppressed if ignored else findings).append({
                    'file': str(filename), 'line': location['line'], 'check': check,
                    'description': finding['description']})
        code = proc.returncode or int(bool(findings))
        if not target.exists() and not code:
            code = 2  # Successful exit without a report is incomplete analysis.
        return {'file': name, 'returncode': code, 'tool_returncode': proc.returncode,
                'findings': findings, 'explicitly_suppressed': suppressed,
                'output': proc.stdout + proc.stderr}
    if not entries:
        raise RuntimeError('no translation units selected for analysis')
    results = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for index, result in enumerate(pool.map(one, entries), 1):
            results.append(result)
            if index % 25 == 0:
                print(f'Clang Static Analyzer: {index}/{len(entries)} checked', flush=True)
    for result in results:
        if result['output']:
            print(result['file'], result['output'], flush=True)
    (logs / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'Clang Static Analyzer: {len(results)} translation units; '
          f'{sum(r["returncode"] != 0 for r in results)} failed', flush=True)
    return int(any(r['returncode'] != 0 for r in results))


def unit_tests(jobs, built):
    results = []
    for compiler in ('clang', 'gcc'):
        if not built.get(compiler):
            results.append({'compiler': compiler, 'blocked': 'compiler build failed'})
            continue
        tree = OUTPUT / compiler
        commands = native_commands(tree, jobs)

        def check(item):
            index, command = item
            started = time.monotonic()
            with (OUTPUT / 'logs' / f'unit-{compiler}-{index:03d}.log').open('w') as log:
                try:
                    proc = subprocess.run(command, cwd=tree, stdout=log, stderr=subprocess.STDOUT,
                                          timeout=UNIT_TIMEOUT_SECONDS, check=False)
                    code = proc.returncode
                except subprocess.TimeoutExpired:
                    code = 124
            return {'compiler': compiler, 'command': command, 'returncode': code,
                    'seconds': round(time.monotonic() - started, 2)}

        # Native fixtures own separate temporary directories and sockets, as in
        # the existing regression runner. Keep compiler families sequential;
        # bound parallel processes within each family by the requested job count.
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for index, result in enumerate(pool.map(check, enumerate(commands, 1)), 1):
                results.append(result)
                print(f'{compiler} unit {index}: {"PASS" if result["returncode"] == 0 else "FAIL"} '
                      f'{shlex.join(result["command"])}', flush=True)
    (OUTPUT / 'unit-results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(any(r.get('returncode', 1) != 0 for r in results))


def integration_tests(log, built):
    """Run current contracts, borrowing the caller's open stage log."""
    results = []
    output = OUTPUT / 'integration-results.json'
    output.write_text('[]\n')
    if not built.get('clang'):
        output.write_text(json.dumps([{'blocked': 'clang build failed', 'returncode': 1}]) + '\n')
        return 1
    tree = OUTPUT / 'clang'
    recipe = run(['make', '--no-print-directory', '-n', '-o', 'test-prereqs', 'test-current-contracts'], cwd=tree)
    commands = [shlex.split(line) for line in recipe.splitlines() if line.startswith('python3 ')]
    if not commands or len({tuple(command) for command in commands}) != len(commands):
        raise RuntimeError('empty or duplicate current-contract integration selection')
    for command in commands:
        started = time.monotonic()
        log.flush()
        process = subprocess.Popen(command, cwd=tree, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            code = process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            code = 124
        results.append({'command': command, 'returncode': code,
                        'seconds': round(time.monotonic() - started, 2)})
        print(f'current contract: {"PASS" if code == 0 else "FAIL"} {shlex.join(command)}', flush=True)
        output.write_text(json.dumps(results, indent=2) + '\n')
    return int(any(result['returncode'] != 0 for result in results))


def execute_stage(stage, jobs, log, built, standalone=False):
    if stage in ('clang-build', 'gcc-build'):
        compiler = stage.split('-')[0]
        tree = prepare_tree(compiler)
        code = logged(make_command(compiler, jobs) + ['test-prereqs'], log, tree)
        if code == 0 and compiler == 'clang':
            code = logged(make_command(compiler, jobs) + ['check-production-binary'], log, tree)
        built[compiler] = code == 0
    elif stage == 'lint':
        code = logged(['make', '-k', 'CC=clang-18', 'CXX=clang++-18',
                       f'CXXFLAGS={CXXFLAGS} -stdlib=libc++',
                       'verify-tool-tests', 'check-network-catalog', 'check-api-docs',
                       'check-redaction-tables', 'check-layers',
                       'check-cpp-contracts', 'lint-all'], log)
    elif stage in ('cppcheck', 'clang-analyzer'):
        entries = compilation_database(jobs)
        if stage == 'clang-analyzer':
            # Worker output is written separately; stage summary is
            # persisted in report.json and analyzer/results.json.
            analyzer_logs = OUTPUT / 'logs/analyzer'
            analyzer_logs.mkdir(exist_ok=True)
            code = analyze(entries, jobs, analyzer_logs)
        else:
            (OUTPUT / 'cppcheck-cache').mkdir(exist_ok=True)
            code = logged(['cppcheck', '--project=' + str(OUTPUT / 'compile_commands.json'),
                           '--enable=warning,performance,portability', '--inline-suppr',
                           '--error-exitcode=1', '--std=c++23', '--platform=unix64',
                           '-D__EXCEPTIONS=1', '-D__cpp_exceptions=199711L',
                           '--suppressions-list=.cppcheck-suppressions',
                           '--cppcheck-build-dir=' + str(OUTPUT / 'cppcheck-cache'),
                           '--template=gcc', f'-j{jobs}'], log)
    elif stage == 'integration-tests':
        if standalone:
            tree = prepare_tree('clang')
            built['clang'] = logged(make_command('clang', jobs) + ['test-prereqs'], log, tree) == 0
        code = integration_tests(log, built)
    else:
        # Standalone invocation also compiles first; stale binaries
        # must never produce a successful unit-test stage.
        if standalone:
            for compiler in ('clang', 'gcc'):
                tree = prepare_tree(compiler)
                built[compiler] = logged(make_command(compiler, jobs) + ['test-prereqs'], log, tree) == 0
        code = unit_tests(jobs, built)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=('all', 'native', 'lean', *STAGES), default='all',
                        help='native groups compiler builds and their tests for parallel CI; '
                             'lean is one compiler plus lint')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 8))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    if not os.environ.get('FORTIS_VERIFY_CONTAINER'):
        return docker(args.stage, args.jobs)
    OUTPUT.mkdir(exist_ok=True)
    with (OUTPUT / '.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('another verification command is using build-verify/')
        return run_gate(args)


def run_gate(args):
    logs = OUTPUT / 'logs'
    logs.mkdir(exist_ok=True)
    # Advisory lint environment variables must never weaken the pre-push gate.
    env = dict(os.environ, FORTIS_LINT_RUNTIME='host', CLANG_TIDY='clang-tidy-18',
               CLANG_FORMAT='clang-format-18', FORTIS_LINT_EXTRA_FLAGS='-stdlib=libc++',
               FORTIS_LINT_REPORT_ONLY='0', FORTIS_LINT_JOBS=str(args.jobs))
    os.environ.update(env)
    built = {}
    report = {'complete': False, 'stages': {}, 'tool_versions': {}, 'started_at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    report_path = OUTPUT / ('report.json' if args.stage == 'all' else f'report-{args.stage}.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    for tool in ('clang-18', 'gcc-13', 'clang-tidy-18', 'cppcheck', 'shellcheck'):
        version_lines = run([tool, '--version']).splitlines()
        report['tool_versions'][tool] = (next(line for line in version_lines if line.startswith('version:'))
                                         if tool == 'shellcheck' else version_lines[0])
    commit, diff = checkout_provenance()
    report['commit'] = os.environ.get('FORTIS_VERIFY_COMMIT', commit)
    report['diff_sha256'] = os.environ.get('FORTIS_VERIFY_DIFF_SHA256', diff)
    # The verified upstream assembly lives in an ignored vendor subdirectory.
    # Populate it before freezing inputs; otherwise a fresh compiler build
    # creates source files inside third_party/ and invalidates its own snapshot.
    with (logs / 'bootstrap.log').open('w') as log:
        bootstrap_code = logged(['sh', 'scripts/fetch-blst-assembly.sh'], log)
    report['bootstrap'] = {'returncode': bootstrap_code}
    if bootstrap_code:
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        return bootstrap_code
    report['input_sha256'] = input_hashes()
    try:
        selected = (STAGES if args.stage == 'all' else
                    NATIVE_STAGES if args.stage == 'native' else
                    LEAN_STAGES if args.stage == 'lean' else (args.stage,))
        for stage in selected:
            start = time.monotonic()
            print(f'\nverify: starting {stage}', flush=True)
            result_path = OUTPUT / RESULT_FILES[stage] if stage in RESULT_FILES else None
            if result_path:
                result_path.unlink(missing_ok=True)
            with (logs / f'{stage}.log').open('w') as log:
                # Stream stage output to its log and console without losing the
                # real exit code through a shell pipeline.
                try:
                    code = execute_stage(stage, args.jobs, log, built, args.stage not in ('all', 'native', 'lean'))
                except (OSError, RuntimeError, ValueError, plistlib.InvalidFileException, subprocess.CalledProcessError) as error:
                    code = 2
                    log.write(str(error) + '\n')
                    print(f'verify: {stage}: {error}', file=sys.stderr)
            if result_path and result_path.exists():
                try:
                    data = result_path.read_bytes()
                    rows = json.loads(data)
                    if not isinstance(rows, list):
                        raise ValueError('test results must be an array')
                    report.setdefault('test_results', {})[stage] = {
                        'sha256': hashlib.sha256(data).hexdigest(), 'count': len(rows)}
                except (OSError, ValueError) as error:
                    code = 2
                    print(f'verify: {stage}: invalid result artifact: {error}', file=sys.stderr)
            report['stages'][stage] = {'returncode': code, 'seconds': round(time.monotonic() - start, 2)}
            report_path.write_text(json.dumps(report, indent=2) + '\n')
            print(f'verify: {stage}: {"PASS" if code == 0 else "FAIL"}', flush=True)
        after = input_hashes()
        before = report['input_sha256']
        report['inputs_unchanged'] = before == after
        if not report['inputs_unchanged']:
            changes = {
                'added': sorted(after.keys() - before.keys()),
                'removed': sorted(before.keys() - after.keys()),
                'modified': sorted(name for name in before.keys() & after.keys() if before[name] != after[name]),
            }
            report['input_changes'] = changes
            print('verify: inputs changed during verification; refusing a passing result', file=sys.stderr)
            for kind, names in changes.items():
                for name in names[:20]:
                    print(f'verify: input {kind}: {name}', file=sys.stderr)
                if len(names) > 20:
                    print(f'verify: {len(names) - 20} more {kind} paths in report.json', file=sys.stderr)
        report['complete'] = True
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    return int(not report['inputs_unchanged'] or
               any(s['returncode'] != 0 for s in report['stages'].values()))


def logged(command, log, cwd=PROJECT):
    with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, errors='replace') as proc:
        for line in proc.stdout:
            log.write(line)
            log.flush()
            print(line, end='', flush=True)
        return proc.wait()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, plistlib.InvalidFileException, subprocess.CalledProcessError) as error:
        print(f'verify: {error}', file=sys.stderr)
        sys.exit(2)
