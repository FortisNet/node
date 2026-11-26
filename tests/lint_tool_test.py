#!/usr/bin/env python3
"""Prove lint findings and tool failures cannot silently pass the gate."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('fortis_lint', ROOT / 'scripts/lint.py')
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)
WARNING = 'src/example.cpp:1:1: warning: use nullptr [modernize-use-nullptr]\n'


class LintGateTest(unittest.TestCase):
    def result(self, status, diagnostics, source='src/example.cpp'):
        with tempfile.TemporaryDirectory(prefix='fortis-lint-gate-') as directory:
            project = Path(directory)
            (project / '.deps/mbedtls-3.6.7/include').mkdir(parents=True)
            output = io.StringIO()

            def run(command, **_kwargs):
                if command[0] == 'make':
                    return subprocess.CompletedProcess(command, 0, '', '')
                return subprocess.CompletedProcess(command, status, diagnostics, '')

            with patch.object(lint, 'PROJECT', project), \
                    patch.object(lint, 'compile_lines', return_value={}), \
                    patch.object(lint.subprocess, 'run', side_effect=run), \
                    patch.dict(os.environ, {'FORTIS_LINT_REPORT_ONLY': '0', 'FORTIS_LINT_JOBS': '1'}), \
                    contextlib.redirect_stdout(output):
                code = lint.tidy([source])
            return code, output.getvalue()

    def test_changed_file_lint_supports_main_master_and_initial_trunk(self):
        for branch in ('main', 'master', 'trunk'):
            with self.subTest(branch=branch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                def git(*args):
                    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
                git('init', '-q', '-b', branch)
                git('config', 'user.name', 'Lint fixture')
                git('config', 'user.email', 'fixture@example.invalid')
                git('config', 'commit.gpgsign', 'false')
                (root / 'src').mkdir()
                (root / 'src/example.cpp').write_text('int value = 0;\n')
                git('add', '.')
                git('commit', '-qm', 'fixture')
                (root / 'src/example.cpp').write_text('int value = 1;\n')
                (root / 'src/new.cpp').write_text('int other = 0;\n')
                with patch.object(lint, 'PROJECT', root):
                    self.assertEqual(lint.changed_files(None), ['src/example.cpp', 'src/new.cpp'])
                    self.assertEqual(lint.changed_lines(None), {'src/example.cpp': [1], 'src/new.cpp': None})

    def test_clean_analysis_passes(self):
        self.assertEqual(self.result(0, '')[0], 0)

    def test_warning_fails(self):
        self.assertEqual(self.result(0, WARNING)[0], 1)

    def test_failure_with_warning_is_analysis_error(self):
        self.assertEqual(self.result(7, WARNING)[0], 2)

    def test_unparsed_failure_is_analysis_error(self):
        self.assertEqual(self.result(9, 'tool crashed')[0], 2)

    def test_compiler_error_is_analysis_error(self):
        error = 'src/example.cpp:1:1: error: invalid C++ [clang-diagnostic-error]\n'
        self.assertEqual(self.result(1, error)[0], 2)

    def test_header_paths_are_deduplicated(self):
        duplicate = WARNING.replace('src/example.cpp', 'tests/../src/example.cpp')
        code, output = self.result(0, WARNING + duplicate)
        self.assertEqual(code, 1)
        self.assertIn('1 files, 1 findings', output)

    def test_missing_test_build_flags_fail(self):
        self.assertEqual(self.result(0, '', 'tests/example.cpp')[0], 2)


if __name__ == '__main__':
    unittest.main()
