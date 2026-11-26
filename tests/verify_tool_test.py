"""Regression tests for verification coverage and failure propagation."""
import json
import hashlib
import io
import os
import plistlib
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import verify
import verify_ci


class VerifyTests(unittest.TestCase):
    def test_real_gcc_and_strict_warning_flags(self):
        command = verify.make_command('gcc', 2)
        self.assertIn('CC=gcc-13', command)
        self.assertIn('CXX=g++-13', command)
        flags = next(c for c in command if c.startswith('CXXFLAGS='))
        for flag in ('-Werror', '-Wformat=2', '-Wundef', '-fno-rtti'):
            self.assertIn(flag, flags)

    def test_input_snapshot_covers_contracts_component_manifest_and_vendor_code(self):
        snapshot = verify.input_hashes()
        for name in ('.gitignore', '.dockerignore',
                     'Dockerfile.native-build',
                     'component_headers.json', 'openapi.native.yaml',
                     'third_party/nanopb/pb_common.c', 'scripts/check_memory_ownership.py'):
            self.assertIn(name, snapshot)
        self.assertFalse(any('.DS_Store' in name or '__pycache__' in name for name in snapshot))

    def test_compile_flag_exit_becomes_stage_failure(self):
        with patch.object(verify.lint, 'compile_lines', side_effect=SystemExit(2)):
            with self.assertRaisesRegex(RuntimeError, 'compilation flags'):
                verify.compilation_database(1)

    def test_native_selection_preserves_arguments(self):
        with patch.object(verify, 'run', return_value='./build/check "a b.json"\npython3 replay.py\n'):
            self.assertEqual(verify.native_commands(Path('/tmp/clang'), 2),
                             [['./build/check', 'a b.json']])

    def test_empty_or_duplicate_tests_fail_closed(self):
        for recipe in ('python3 replay.py\n', './build/a\n./build/a\n'):
            with patch.object(verify, 'run', return_value=recipe):
                with self.assertRaises(RuntimeError):
                    verify.native_commands(Path('/tmp/clang'), 1)

    def test_failed_clang_build_blocks_current_contracts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(verify, 'OUTPUT', root), patch.object(verify, 'run') as select:
                self.assertEqual(verify.integration_tests(root / 'integration.log', {'clang': False}), 1)
                select.assert_not_called()
                self.assertEqual(json.loads((root / 'integration-results.json').read_text())[0]['returncode'], 1)

    def test_current_contracts_borrow_stream_and_keep_results_after_failure(self):
        for exit_code in (0, 7):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'clang').mkdir()
                recipe = (f"python3 -c 'import sys; sys.exit({exit_code})'\n"
                          "python3 -c 'print(\"current-contract-finished\")'\n")
                path = root / 'integration.log'
                with path.open('w') as log, patch.object(verify, 'OUTPUT', root), \
                        patch.object(verify, 'run', return_value=recipe):
                    self.assertEqual(verify.integration_tests(log, {'clang': True}), int(exit_code != 0))
                    self.assertFalse(log.closed)
                self.assertIn('current-contract-finished', path.read_text())
                results = json.loads((root / 'integration-results.json').read_text())
                self.assertEqual([row['returncode'] for row in results], [exit_code, 0])

    def test_failed_build_blocks_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(verify, 'OUTPUT', root), patch.object(verify, 'native_commands') as select:
                self.assertEqual(verify.unit_tests(1, {'clang': False, 'gcc': False}), 1)
                select.assert_not_called()
                self.assertEqual(len(json.loads((root / 'unit-results.json').read_text())), 2)

    def test_unit_jobs_run_independent_fixtures_concurrently(self):
        barrier = threading.Barrier(2)

        def complete(*args, **kwargs):
            barrier.wait(timeout=5)
            return SimpleNamespace(returncode=0)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'logs').mkdir()
            (root / 'clang').mkdir()
            (root / 'gcc').mkdir()
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'native_commands', return_value=[['a'], ['b']]), \
                    patch.object(verify.subprocess, 'run', side_effect=complete), \
                    patch.object(sys, 'stdout', io.StringIO()):
                self.assertEqual(verify.unit_tests(2, {'clang': True, 'gcc': True}), 0)
                rows = json.loads((root / 'unit-results.json').read_text())
                self.assertEqual(len(rows), 4)
                self.assertEqual([r['compiler'] for r in rows], ['clang', 'clang', 'gcc', 'gcc'])

    def test_failed_unit_cannot_be_baselined_away(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'logs').mkdir()
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'native_commands', return_value=[['/usr/bin/false']]):
                (root / 'clang').mkdir()
                (root / 'gcc').mkdir()
                self.assertEqual(verify.unit_tests(1, {'clang': True, 'gcc': True}), 1)

    def test_timed_out_unit_is_a_failure_for_both_compilers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'logs').mkdir()
            (root / 'clang').mkdir()
            (root / 'gcc').mkdir()
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'native_commands', return_value=[['check']]), \
                    patch.object(verify.subprocess, 'run', side_effect=verify.subprocess.TimeoutExpired('check', 600)), \
                    patch.object(sys, 'stdout', io.StringIO()):
                self.assertEqual(verify.unit_tests(1, {'clang': True, 'gcc': True}), 1)
            rows = json.loads((root / 'unit-results.json').read_text())
            self.assertEqual([row['returncode'] for row in rows], [124, 124])

    def test_analyzer_tool_failure_is_fatal(self):
        with tempfile.TemporaryDirectory() as directory:
            entry = {'file': 'missing.cpp', 'arguments': ['/usr/bin/false', '-c', 'missing.cpp']}
            self.assertEqual(verify.analyze([entry], 1, Path(directory)), 1)

    def test_changed_toolchain_invalidates_only_its_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / 'project'
            project.mkdir()
            (project / 'Dockerfile.verify').write_text('toolchain')
            output = project / 'build-verify'
            with patch.object(verify, 'PROJECT', project), patch.object(verify, 'OUTPUT', output), \
                    patch.object(verify, 'run', return_value='compiler version 1'):
                tree = verify.prepare_tree('clang')
                (tree / 'build').mkdir()
                (tree / 'build/old.o').write_text('old object')
                verify.prepare_tree('clang')
                self.assertTrue((tree / 'build/old.o').exists())
            with patch.object(verify, 'PROJECT', project), patch.object(verify, 'OUTPUT', output), \
                    patch.object(verify, 'run', return_value='compiler version 2'):
                verify.prepare_tree('clang')
                self.assertFalse((tree / 'build').exists())
                self.assertTrue((project / 'Dockerfile.verify').exists())

    def test_analyzer_findings_and_explicit_annotations(self):
        for comment, expected in [('', 1), (' // NOLINT(clang-analyzer-cplusplus.Move)', 0),
                                  (' // NOLINT(other-check)', 1)]:
            with self.subTest(comment=comment), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / 'case.cpp'
                source.write_text('int value;' + comment + '\n')
                entry = {'file': 'case.cpp', 'arguments': ['clang++-18', '-c', 'case.cpp']}
                def compiler(*args, **kwargs):
                    document = {'files': [str(source)], 'diagnostics': [{
                        'location': {'file': 0, 'line': 1}, 'check_name': 'cplusplus.Move',
                        'description': 'moved-from test'}]}
                    (root / 'case.cpp.plist').write_bytes(plistlib.dumps(document))
                    return SimpleNamespace(returncode=0, stdout='', stderr='')
                with patch.object(verify.subprocess, 'run', side_effect=compiler):
                    self.assertEqual(verify.analyze([entry], 1, root), expected)

    def test_analyzer_requires_a_fresh_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'case.cpp.plist').write_bytes(plistlib.dumps({'files': [], 'diagnostics': []}))
            entry = {'file': 'case.cpp', 'arguments': ['/usr/bin/true', '-c', 'case.cpp']}
            self.assertEqual(verify.analyze([entry], 1, root), 1)

    def test_all_stages_continue_after_tool_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'run', return_value='tool version\nversion: fixture-shellcheck-1\n'), \
                    patch.object(verify, 'logged', return_value=0), \
                    patch.object(verify.subprocess, 'check_output', return_value=b''), \
                    patch.object(verify, 'execute_stage', side_effect=[OSError('tool failed')] + [0] * (len(verify.STAGES) - 1)) as stage, \
                    patch.object(sys, 'argv', ['verify.py']), \
                    patch.dict(os.environ, {'FORTIS_VERIFY_CONTAINER': '1', 'FORTIS_LINT_REPORT_ONLY': '1'}), \
                    patch.object(sys, 'stdout', io.StringIO()), patch.object(sys, 'stderr', io.StringIO()):
                self.assertEqual(verify.main(), 1)
                self.assertEqual(stage.call_count, len(verify.STAGES))
                self.assertEqual(os.environ['FORTIS_LINT_REPORT_ONLY'], '0')
                report = json.loads((root / 'report.json').read_text())
                self.assertTrue(report['complete'])
                self.assertEqual(report['stages']['clang-build']['returncode'], 2)

    def test_fresh_vendor_bootstrap_precedes_the_frozen_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assembly = root / 'downloaded-assembly.S'

            def bootstrap(command, log):
                self.assertEqual(command, ['sh', 'scripts/fetch-blst-assembly.sh'])
                assembly.write_text('verified upstream assembly')
                return 0

            def snapshot():
                self.assertTrue(assembly.exists(), 'vendor source must exist before fingerprinting')
                return {'assembly.S': assembly.read_text()}

            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'run', return_value='tool version\nversion: fixture-shellcheck-1\n'), \
                    patch.object(verify.subprocess, 'check_output', return_value=b''), \
                    patch.object(verify, 'logged', side_effect=bootstrap), \
                    patch.object(verify, 'input_hashes', side_effect=snapshot), \
                    patch.object(verify, 'execute_stage', return_value=0), \
                    patch.object(sys, 'argv', ['verify.py']), \
                    patch.dict(os.environ, {'FORTIS_VERIFY_CONTAINER': '1'}), \
                    patch.object(sys, 'stdout', io.StringIO()):
                self.assertEqual(verify.main(), 0)
            report = json.loads((root / 'report.json').read_text())
            self.assertTrue(report['complete'] and report['inputs_unchanged'])
            self.assertEqual(report['tool_versions']['shellcheck'], 'version: fixture-shellcheck-1')
            self.assertEqual(report['bootstrap']['returncode'], 0)

    def test_failed_vendor_bootstrap_blocks_snapshot_and_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'run', return_value='tool version\nversion: fixture-shellcheck-1\n'), \
                    patch.object(verify.subprocess, 'check_output', return_value=b''), \
                    patch.object(verify, 'logged', return_value=9), \
                    patch.object(verify, 'input_hashes') as snapshot, \
                    patch.object(verify, 'execute_stage') as stage, \
                    patch.object(sys, 'argv', ['verify.py']), \
                    patch.dict(os.environ, {'FORTIS_VERIFY_CONTAINER': '1'}):
                self.assertEqual(verify.main(), 9)
            snapshot.assert_not_called()
            stage.assert_not_called()
            report = json.loads((root / 'report.json').read_text())
            self.assertFalse(report['complete'])
            self.assertEqual(report['bootstrap']['returncode'], 9)

    def test_input_drift_fails_with_paths_even_when_every_stage_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshots = [{'changed.cpp': 'before', 'removed.cpp': 'old'},
                         {'changed.cpp': 'after', 'added.cpp': 'new'}]
            errors = io.StringIO()
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'run', return_value='tool version\nversion: fixture-shellcheck-1\n'), \
                    patch.object(verify.subprocess, 'check_output', return_value=b''), \
                    patch.object(verify, 'logged', return_value=0), \
                    patch.object(verify, 'input_hashes', side_effect=snapshots), \
                    patch.object(verify, 'execute_stage', return_value=0), \
                    patch.object(sys, 'argv', ['verify.py']), \
                    patch.dict(os.environ, {'FORTIS_VERIFY_CONTAINER': '1'}), \
                    patch.object(sys, 'stdout', io.StringIO()), patch.object(sys, 'stderr', errors):
                self.assertEqual(verify.main(), 1)
            report = json.loads((root / 'report.json').read_text())
            self.assertTrue(report['complete'])
            self.assertFalse(report['inputs_unchanged'])
            self.assertEqual(report['input_changes'], {
                'added': ['added.cpp'], 'removed': ['removed.cpp'], 'modified': ['changed.cpp']})
            for name in ('changed.cpp', 'removed.cpp', 'added.cpp'):
                self.assertIn(name, errors.getvalue())


class ParallelCiTests(unittest.TestCase):
    commit = 'a' * 40

    def make_reports(self, root):
        for group, stages in verify_ci.GROUPS.items():
            directory = root / group
            directory.mkdir()
            report = {'complete': True, 'inputs_unchanged': True,
                      'commit': self.commit, 'diff_sha256': verify_ci.CLEAN_DIFF,
                      'bootstrap': {'returncode': 0}, 'input_sha256': {'src/a.cpp': 'b' * 64},
                      'tool_versions': {'clang-18': 'version'},
                      'stages': {stage: {'returncode': 0, 'seconds': 1} for stage in stages}}
            if group == 'native':
                report['test_results'] = {}
                for stage, filename in verify.RESULT_FILES.items():
                    rows = ([{'compiler': compiler, 'command': ['./build/check'], 'returncode': 0}
                             for compiler in ('clang', 'gcc')] if stage == 'unit-tests' else
                            [{'command': ['python3', 'test.py'], 'returncode': 0}])
                    data = (json.dumps(rows) + '\n').encode()
                    (directory / filename).write_bytes(data)
                    report['test_results'][stage] = {'sha256': hashlib.sha256(data).hexdigest(),
                                                     'count': len(rows)}
            (directory / f'report-{group}.json').write_text(json.dumps(report))

    def test_parallel_groups_cover_the_original_gate_once(self):
        stages = [stage for values in verify_ci.GROUPS.values() for stage in values]
        self.assertEqual(set(stages), set(verify.STAGES))
        self.assertEqual(len(stages), len(verify.STAGES))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_reports(root)
            report, results = verify_ci.merge(root, self.commit)
            self.assertTrue(report['complete'] and report['inputs_unchanged'])
            self.assertEqual(set(report['stages']), set(verify.STAGES))
            self.assertEqual(len(json.loads(results['unit-results.json'])), 2)

    def test_incomplete_failed_dirty_or_mixed_reports_fail_closed(self):
        changes = [({'complete': False}, 'incomplete'),
                   ({'inputs_unchanged': False}, 'changed inputs'),
                   ({'bootstrap': {'returncode': 9}}, 'bootstrap'),
                   ({'commit': 'wrong'}, 'commit'),
                   ({'diff_sha256': 'dirty'}, 'dirty'),
                   ({'stages': {}}, 'stages'),
                   ({'stages': {'lint': {'returncode': 1}}}, 'failed stages'),
                   ({'input_sha256': {'src/a.cpp': 'changed'}}, 'differ'),
                   ({'tool_versions': {'clang-18': 'different'}}, 'differ')]
        for changeset, message in changes:
            with self.subTest(changes=changeset), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.make_reports(root)
                path = root / 'lint/report-lint.json'
                report = json.loads(path.read_text())
                report.update(changeset)
                path.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, message):
                    verify_ci.merge(root, self.commit)

    def test_missing_or_duplicate_group_cannot_pass(self):
        for duplicate in (False, True):
            with self.subTest(duplicate=duplicate), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.make_reports(root)
                report = root / 'lint/report-lint.json'
                if duplicate:
                    (root / 'report-lint.json').write_bytes(report.read_bytes())
                else:
                    report.unlink()
                with self.assertRaisesRegex(ValueError, 'exactly one'):
                    verify_ci.merge(root, self.commit)

    def test_changed_or_incomplete_test_results_cannot_pass(self):
        mutations = [lambda rows: rows[:1],
                     lambda rows: rows + rows[:1],
                     lambda rows: [dict(row, returncode=7) for row in rows],
                     lambda rows: []]
        for mutate in mutations:
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.make_reports(root)
                path = root / 'native/unit-results.json'
                path.write_text(json.dumps(mutate(json.loads(path.read_text()))))
                with self.assertRaisesRegex(ValueError, 'altered test results'):
                    verify_ci.merge(root, self.commit)

    def test_failed_results_or_compiler_gaps_fail_even_with_matching_digest(self):
        mutations = [(lambda rows: rows[:1], 'different coverage'),
                     (lambda rows: rows + rows[:1], 'duplicate'),
                     (lambda rows: [dict(row, returncode=7) for row in rows], 'failed or blocked')]
        for mutate, message in mutations:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.make_reports(root)
                path = root / 'native/unit-results.json'
                rows = mutate(json.loads(path.read_text()))
                data = json.dumps(rows).encode()
                path.write_bytes(data)
                report_path = root / 'native/report-native.json'
                report = json.loads(report_path.read_text())
                report['test_results']['unit-tests'] = {'sha256': hashlib.sha256(data).hexdigest(),
                                                        'count': len(rows)}
                report_path.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, message):
                    verify_ci.merge(root, self.commit)

    def test_cli_writes_the_combined_report_and_verified_test_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_reports(root)
            output = root / 'output'
            with patch.object(sys, 'argv', ['verify_ci.py', str(root), '--commit', self.commit,
                                          '--output', str(output)]), \
                    patch.object(sys, 'stdout', io.StringIO()):
                self.assertEqual(verify_ci.main(), 0)
            report = json.loads((output / 'report.json').read_text())
            self.assertTrue(report['complete'] and report['inputs_unchanged'])
            for stage, name in verify.RESULT_FILES.items():
                data = (output / name).read_bytes()
                self.assertEqual(report['test_results'][stage]['sha256'], hashlib.sha256(data).hexdigest())

    def test_ci_job_failure_writes_a_failed_aggregate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_reports(root)
            output = root / 'output'
            with patch.object(sys, 'argv', ['verify_ci.py', str(root), '--commit', self.commit,
                                          '--jobs-result', 'failure', '--output', str(output)]), \
                    patch.object(sys, 'stderr', io.StringIO()):
                self.assertEqual(verify_ci.main(), 1)
            self.assertFalse(json.loads((output / 'report.json').read_text())['complete'])

    def test_invalid_result_artifact_fails_without_skipping_later_stages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def execute(stage, *args):
                if stage == 'unit-tests':
                    (root / 'unit-results.json').write_text('invalid JSON')
                return 0

            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'run', return_value='tool version\nversion: fixture-shellcheck-1\n'), \
                    patch.object(verify, 'logged', return_value=0), \
                    patch.object(verify.subprocess, 'check_output', return_value=b''), \
                    patch.object(verify, 'execute_stage', side_effect=execute) as run_stage, \
                    patch.object(sys, 'argv', ['verify.py']), \
                    patch.dict(os.environ, {'FORTIS_VERIFY_CONTAINER': '1'}), \
                    patch.object(sys, 'stdout', io.StringIO()), patch.object(sys, 'stderr', io.StringIO()):
                self.assertEqual(verify.main(), 1)
            self.assertEqual(run_stage.call_count, len(verify.STAGES))
            report = json.loads((root / 'report.json').read_text())
            self.assertTrue(report['complete'])
            self.assertEqual(report['stages']['unit-tests']['returncode'], 2)
            self.assertEqual(report['stages']['integration-tests']['returncode'], 0)

    def test_native_group_reuses_successful_builds_for_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(verify, 'OUTPUT', root), \
                    patch.object(verify, 'run', return_value='tool version\nversion: fixture-shellcheck-1\n'), \
                    patch.object(verify, 'logged', return_value=0), \
                    patch.object(verify.subprocess, 'check_output', return_value=b''), \
                    patch.object(verify, 'execute_stage', return_value=0) as execute, \
                    patch.object(sys, 'argv', ['verify.py', '--stage', 'native']), \
                    patch.dict(os.environ, {'FORTIS_VERIFY_CONTAINER': '1'}), \
                    patch.object(sys, 'stdout', io.StringIO()):
                self.assertEqual(verify.main(), 0)
            self.assertEqual([call.args[0] for call in execute.call_args_list], list(verify.NATIVE_STAGES))
            self.assertTrue(all(call.args[-1] is False for call in execute.call_args_list))
            self.assertTrue((root / 'report-native.json').is_file())


if __name__ == '__main__':
    unittest.main()
